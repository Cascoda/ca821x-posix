
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include <signal.h>

#include "../ca821x-posix.h"

/* Colour codes for printf */
#ifndef NO_COLOR
#define RED        "\x1b[31m"
#define GREEN      "\x1b[32m"
#define YELLOW     "\x1b[33m"
#define BLUE       "\x1b[34m"
#define MAGENTA    "\x1b[35m"
#define CYAN       "\x1b[36m"
#define BOLDWHITE  "\033[1m\033[37m"
#define RESET      "\x1b[0m"
#else
#define RED        ""
#define GREEN      ""
#define YELLOW     ""
#define BLUE       ""
#define MAGENTA    ""
#define CYAN       ""
#define BOLDWHITE  ""
#define RESET      ""
#endif

#define COLOR_SET(C,X) C X RESET

#define M_PANID 0x1AAA
#define M_MSDU_LENGTH 100
#define MAX_INSTANCES 3
#define TX_PERIOD_US 50000

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
struct inst_priv insts[MAX_INSTANCES] = {};

pthread_mutex_t out_mutex = PTHREAD_MUTEX_INITIALIZER;

void initInst(struct inst_priv *cur);

static void quit(int sig)
{
	for(int i = 0; i < numInsts; i++){
		pthread_cancel(insts[i].mWorker);
		pthread_join(insts[i].mWorker, NULL);
	}
	exit(0);
}

static int driverErrorCallback(int error_number, struct ca821x_dev *pDeviceRef)
{
	struct inst_priv *priv = pDeviceRef->context;
	pthread_mutex_t *confirm_mutex = &(priv->confirm_mutex);
	pthread_cond_t *confirm_cond = &(priv->confirm_cond);

	printf( COLOR_SET(RED,"DRIVER FAILED FOR %x WITH ERROR %d\n\r") , priv->mAddress, error_number);
	printf( COLOR_SET(BLUE,"Attempting restart...\n\r"));

	initInst(priv);

	printf( COLOR_SET(GREEN,"Restart successful!\n\r"));

	pthread_mutex_lock(confirm_mutex);
	priv->confirm_done = 1;
	pthread_cond_broadcast(confirm_cond);
	pthread_mutex_unlock(confirm_mutex);

	return 0;
}

int handleUserCallback(const uint8_t *buf, size_t len,
                       struct ca821x_dev *pDeviceRef)
{
	struct inst_priv *priv = pDeviceRef->context;

	if (buf[0] == 0xA0)
	{
		fprintf(stderr, "IN %04x: %.*s\n", priv->mAddress, len - 2, buf + 2);
		return 1;
	}
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

	TDME_SETSFR_request_sync(0, 0xdb, 0x0A, pDeviceRef);

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

		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		usleep(TX_PERIOD_US);

		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		//fire
		dest.ShortAddress = insts[i].mAddress;
		TDME_SETSFR_request_sync(0, 0xdb, 0x0E, pDeviceRef);
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
		printf("|---" COLOR_SET(BOLDWHITE,"NODE %02d") "---|", i);
	}
	printf("\n");
	printf("|----|");
	for (int i = 0; i < numInsts; i++)
	{
		uint8_t len = 0;
		uint8_t leArr[2];
		MLME_GET_request_sync(macShortAddress, 0, &len, leArr, &insts[i].pDeviceRef);
		printf("|-ShAddr %04x-|", GETLE16(leArr));
	}
	printf("\n");
	printf("|TIME|");
	for(int i = 0; i < numInsts; i++)
	{
		printf("|"COLOR_SET(GREEN,"Tx  ")"|Rx  |"COLOR_SET(RED,"Err")"|");
	}
	printf("\n");
}

void drawTableRow(unsigned int time)
{
	printf("|%4d|", time);
	pthread_mutex_lock(&out_mutex);
	for(int i = 0; i < numInsts; i++)
	{
		printf("|" COLOR_SET(GREEN,"%4d") "|%4d|" COLOR_SET(RED,"%3d") "|",
		       insts[i].mTx, insts[i].mRx, insts[i].mErr);
	}
	pthread_mutex_unlock(&out_mutex);
	printf("\n");
}

void initInst(struct inst_priv *cur)
{
	struct ca821x_dev *pDeviceRef = &(cur->pDeviceRef);

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
		exchange_register_user_callback(&handleUserCallback, pDeviceRef);

		initInst(cur);
		printf("Initialised. %d\r\n", i);
	}

	for(int i = 0; i < numInsts; i++)
	{
		pthread_create(&(insts[i].mWorker), NULL, &inst_worker, &insts[i]);
	}

	signal(SIGINT, quit);

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

