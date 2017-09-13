
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

#include "../ca821x-posix.h"

#define M_PANID 0x1AAA
#define M_MSDU_LENGTH 100
#define MAX_INSTANCES 3

static uint8_t msdu[M_MSDU_LENGTH] = {1, 2, 3, 4, 5, 6, 7, 0};
static struct SecSpec sSecSpec = {0};

struct inst_priv
{
	struct ca821x_dev pDeviceRef;
	pthread_mutex_t confirm_mutex;
	pthread_cond_t confirm_cond;
	pthread_t mWorker;
	uint8_t confirm_done;
	uint16_t mAddress;
	uint8_t lastHandle;

	unsigned int mTx, mRx, mErr;
};

int numInsts;
struct inst_priv insts[MAX_INSTANCES];

pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;

static int driverErrorCallback(int error_number)
{
	printf( "\r\nDRIVER FAILED WITH ERROR %d\n\r", error_number);
	abort();
	return 0;
}

static int handleDataIndication(struct MCPS_DATA_indication_pset *params, struct ca821x_dev *pDeviceRef)   //Async
{
	struct inst_priv *priv = pDeviceRef->context;
	pthread_mutex_lock(&out_mutex);
	priv->mRx++;
	pthread_mutex_unlock(&out_mutex);
	return 0;
}

static int handleDataConfirm(struct MCPS_DATA_confirm_pset *params, struct ca821x_dev *pDeviceRef)   //Async
{
	struct inst_priv *priv = pDeviceRef->context;
	pthread_mutex_t *confirm_mutex = &(priv->confirm_mutex);
	pthread_cond_t *confirm_cond = &(priv->confirm_cond);

	if(params->Status == MAC_SUCCESS)
	{
		pthread_mutex_lock(&out_mutex);
		priv->mTx++;
		pthread_mutex_unlock(&out_mutex);
	}
	else
	{
		pthread_mutex_lock(&out_mutex);
		priv->mErr++;
		pthread_mutex_unlock(&out_mutex);
	}

	pthread_mutex_lock(confirm_mutex);
	assert(params->MsduHandle == priv->lastHandle);
	priv->confirm_done = 1;
	pthread_cond_broadcast(confirm_cond);
	pthread_mutex_unlock(confirm_mutex);
	return 0;
}

static int handleGenericDispatchFrame(const uint8_t *buf, size_t len, struct ca821x_dev *pDeviceRef)   //Async
{

	/*
	 * This is a debugging function for unhandled incoming MAC data
	 */

	return 0;
}

static void *inst_worker(void *arg)
{
	struct inst_priv *priv = arg;
	struct ca821x_dev *pDeviceRef = &(priv->pDeviceRef);

	pthread_mutex_t *confirm_mutex = &(priv->confirm_mutex);
	pthread_cond_t *confirm_cond = &(priv->confirm_cond);

	uint16_t i = 0;
	while(1)
	{
		union MacAddr dest;

		do{
			i = (i+1) % numInsts;
		} while(&insts[i] == priv);
		//wait for confirm & reset
		pthread_mutex_lock(confirm_mutex);
		while(!priv->confirm_done) pthread_cond_wait(confirm_cond, confirm_mutex);
		priv->confirm_done = 0;
		priv->lastHandle++;
		pthread_mutex_unlock(confirm_mutex);

		usleep(50000);

		//fire
		dest.ShortAddress = insts[i].mAddress;
		MCPS_DATA_request(
				MAC_MODE_SHORT_ADDR,
				MAC_MODE_SHORT_ADDR,
				M_PANID,
				&dest,
				M_MSDU_LENGTH,
				msdu,
				priv->lastHandle,
				0x01,
				&sSecSpec,
				pDeviceRef
				);
	}
	return NULL;
}

void drawTableHeader()
{
	printf("|----|");
	for(int i = 0; i < numInsts; i++)
	{
		printf("|----|----|---|");
	}
	printf("\n");
	printf("|----|");
	for(int i = 0; i < numInsts; i++)
	{
		printf("|---NODE %02d---|", i);
	}
	printf("\n");
	printf("|TIME|");
	for(int i = 0; i < numInsts; i++)
	{
		printf("|Tx  |Rx  |Err|");
	}
	printf("\n");
}

void drawTableRow(unsigned int time)
{
	printf("|%4d|", time);
	pthread_mutex_lock(&out_mutex);
	for(int i = 0; i < numInsts; i++)
	{
		printf("|%4d|%4d|%3d|", insts[i].mTx, insts[i].mRx, insts[i].mErr);
	}
	pthread_mutex_unlock(&out_mutex);
	printf("\n");
}

int main(int argc, char *argv[])
{
	if(argc <= 2) return -1;
	numInsts = argc - 1;

	for(int i = 0; i < numInsts; i++){
		struct inst_priv *cur = &insts[i];
		struct ca821x_dev *pDeviceRef = &(cur->pDeviceRef);
		cur->mAddress = atoi(argv[i+1]);
		cur->confirm_done = 1;	

		pthread_mutex_init(&(cur->confirm_mutex), NULL);
		pthread_cond_init(&(cur->confirm_cond), NULL);	
	
		while(ca821x_util_init(pDeviceRef, &driverErrorCallback))
		{
			sleep(1); //Wait while there isn't a device available to connect
		}
		pDeviceRef->context = cur;

		//Register callbacks for async messages
		struct ca821x_api_callbacks callbacks = {0};
		callbacks.MCPS_DATA_indication = &handleDataIndication;
		callbacks.MCPS_DATA_confirm = &handleDataConfirm;
		callbacks.generic_dispatch = &handleGenericDispatchFrame;
		ca821x_register_callbacks(&callbacks, pDeviceRef);

		//Reset the MAC to a default state
		MLME_RESET_request_sync(1, pDeviceRef);

		uint8_t disable = 0; //Disable low LQI rejection @ MAC Layer
		HWME_SET_request_sync(0x11, 1, &disable, pDeviceRef);

		//Set up MAC pib attributes
		uint8_t retries = 3;	//Retry transmission 3 times if not acknowledged
		MLME_SET_request_sync(
			macMaxFrameRetries,
			0,
			sizeof(retries),
			&retries,
			pDeviceRef);

		retries = 4;	//max 4 CSMA backoffs
		MLME_SET_request_sync(
			macMaxCSMABackoffs,
			0,
			sizeof(retries),
			&retries,
			pDeviceRef);

		uint8_t maxBE = 4;	//max BackoffExponent 4
		MLME_SET_request_sync(
			macMaxBE,
			0,
			sizeof(maxBE),
			&maxBE,
			pDeviceRef);

		uint8_t channel = 22;
		MLME_SET_request_sync(
			phyCurrentChannel,
			0,
			sizeof(channel),
			&channel,
			pDeviceRef);

		uint8_t LEarray[2];
		LEarray[0] = LS0_BYTE(M_PANID);
		LEarray[1] = LS1_BYTE(M_PANID);
		MLME_SET_request_sync(
			macPANId,
			0,
			2,
			LEarray,
			pDeviceRef);

		LEarray[0] = LS0_BYTE(cur->mAddress);
		LEarray[1] = LS1_BYTE(cur->mAddress);
		MLME_SET_request_sync(
			macShortAddress,
			0,
			sizeof(cur->mAddress),
			LEarray,
			pDeviceRef);

		uint8_t rxOnWhenIdle = 1;
		MLME_SET_request_sync( //enable Rx when Idle
			macRxOnWhenIdle,
			0,
			sizeof(rxOnWhenIdle),
			&rxOnWhenIdle,
			pDeviceRef);

		printf("Initialised. %d\r\n", i);
	}

	for(int i = 0; i < numInsts; i++)
	{
		pthread_create(&(insts[i].mWorker), NULL, &inst_worker, &insts[i]);
	}

	//Draw the table onscreen every second
	unsigned int time = 0;
	while(1)
	{
		if((time % 20) == 0) drawTableHeader();
		drawTableRow(time);
		sleep(1);
		time++;
	}

	return 0;
}

