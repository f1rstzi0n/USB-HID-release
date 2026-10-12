#include "main.h"
unsigned char MAIN_ucSerialTxBuff[100];
unsigned char MAIN_ucSerialRxBuff[100];
unsigned char MAIN_ucSerialRxTxFlag = 0;
unsigned char MAIN_ucSerialTxCount=0;
unsigned char MAIN_ucSerialRxCount = 0;
unsigned char MAIN_ucSerialRxTxSize = 0;
Uart1Record Uart1;
