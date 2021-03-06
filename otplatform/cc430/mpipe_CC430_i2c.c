/* Copyright 2010-2011 JP Norair
  *
  * Licensed under the OpenTag License, Version 1.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.indigresso.com/wiki/doku.php?id=opentag:license_1_0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  *
  */
/**
  * @file       /otplatform/cc430/mpipe_CC430_i2c.c
  * @author     JP Norair
  * @version    V1.0
  * @date       1 July 2012
  * @brief      Message Pipe (MPIPE) implementation(s) for CC430
  * @defgroup   MPipe (Message Pipe)
  * @ingroup    MPipe
  *
  * As far as I know, the CC430 has the ability to use the following peripherals
  * for the MPIPE.  This list may change over time as new models of the CC430
  * are released.  This is the I2C implementation.  Speed is 400kbps.
  *     I/F     HW      Impl.   Baudrate Notes
  * 1.  UART    USCI    yes     Keep baudrate <= 115.2 kbps
  * 2.  SPI     USCI    no      Potentially up to 5 Mbps, using SMCLK
  * 3.  I2C     USCI    no      100 or 400 kbps
  * 4.  IrDA    USCI    no      Need more information
  *
  * Design Assumptions:
  * - Using SMCLK at 2.49625 MHz Clock (19.97 MHz / 8)
  * - Using UART0
  * - If changing the input frequency, changes need to be made to implementation
  *   of setspeed function.
  * - If changing to another UART, changes to platform_config_CC430.h and to
  *   some macros in this file will be needed
  *
  * Implemented Mpipe Protocol:
  * The Mpipe protocol is a simple wrapper to NDEF.
  * Legend: [ NDEF Header ] [ NDEF Payload ] [ Seq. Number ] [ CRC16 ]
  * Bytes:        6             <= 255             2             2
  *
  * The protocol includes an ACK/NACK feature.  After receiving a message, the
  * Mpipe send an ACK/NACK.  The "YY" byte is 0 for ACK and non-zero for ACK.
  * Presently, 0x7F is used as the YY NACK value.
  * [ Seq ID ] 0xDD 0x00 0x00 0x02 0x00 0xYY  [ CRC16 ]
  ******************************************************************************
  */


#include "OT_config.h"

#if ((OT_FEATURE(MPIPE) == ENABLED) && defined(MPIPE_I2C))

#include "mpipe.h"
#include "OT_platform.h"

#define UART_CLOSE()        (MPIPE_I2C->CTL1   |= UCSWRST)
#define UART_OPEN()         (MPIPE_I2C->CTL1   &= ~UCSWRST)
#define UART_SET_TXIFG()    (MPIPE_I2C->IFG    |= UCTXIFG)
#define UART_CLEAR_TXIFG()  (MPIPE_I2C->IFG    &= ~UCTXIFG)
#define UART_SET_RXIFG()    (MPIPE_I2C->IFG    |= UCRXIFG)
#define UART_CLEAR_RXIFG()  (MPIPE_I2C->IFG    &= ~UCRXIFG)



// Setup DMA for RX, and enable it
#define MPIPE_DMA_RXCTL_ON          ( DMA_Mode_Single | \
                                      DMA_DestinationInc_Enable | \
                                      DMA_SourceInc_Disable | \
                                      DMA_DestinationDataSize_Byte | \
                                      DMA_SourceDataSize_Byte | \
                                      DMA_TriggerLevel_RisingEdge | \
                                      0x0014 )

// Setup DMA for RX, but disable / do not enable
#define MPIPE_DMA_RXCTL_OFF         ( DMA_Mode_Single | \
                                      DMA_DestinationInc_Enable | \
                                      DMA_SourceInc_Disable | \
                                      DMA_DestinationDataSize_Byte | \
                                      DMA_SourceDataSize_Byte | \
                                      DMA_TriggerLevel_RisingEdge | \
                                      0x0004 )

// Setup DMA for TX, and enable it
#define MPIPE_DMA_TXCTL_ON          ( DMA_Mode_Single | \
                                      DMA_DestinationInc_Disable | \
                                      DMA_SourceInc_Enable | \
                                      DMA_DestinationDataSize_Byte | \
                                      DMA_SourceDataSize_Byte | \
                                      DMA_TriggerLevel_RisingEdge | \
                                      0x0014 )

// Setup DMA for TX, but disable / do not enable
#define MPIPE_DMA_TXCTL_OFF         ( DMA_Mode_Single | \
                                      DMA_DestinationInc_Disable | \
                                      DMA_SourceInc_Enable | \
                                      DMA_DestinationDataSize_Byte | \
                                      DMA_SourceDataSize_Byte | \
                                      DMA_TriggerLevel_RisingEdge | \
                                      0x0004 )

#define MPIPE_DMA_RXCONFIG(DEST, SIZE, ONOFF) \
    do { \
        MPIPE_DMA->SA_L = (ot_u16)&(MPIPE_I2C->RXBUF); \
        MPIPE_DMA->DA_L = (ot_u16)DEST; \
        MPIPE_DMA->SZ   = SIZE; \
        MPIPE_DMA->CTL  = MPIPE_DMA_RXCTL_##ONOFF; \
    } while(0)

///@todo Resolve SIZE+2 workaround for DMA interrupt delay
#define MPIPE_DMA_TXCONFIG(SOURCE, SIZE, ONOFF) \
    do { \
        MPIPE_DMA->SA_L = (ot_u16)SOURCE; \
        MPIPE_DMA->DA_L = (ot_u16)&(MPIPE_I2C->TXBUF); \
        MPIPE_DMA->SZ   = SIZE; \
        MPIPE_DMA->CTL  = MPIPE_DMA_TXCTL_##ONOFF; \
    } while(0)

#define MPIPE_DMAEN(ONOFF)  MPIPE_DMA_##ONOFF
#define MPIPE_DMAIE(ONOFF)  MPIPE_DMAIE_##ONOFF
#define MPIPE_DMA_ON        (MPIPE_DMA->CTL |= 0x0010)
#define MPIPE_DMA_OFF       (MPIPE_DMA->CTL &= ~0x0010)
#define MPIPE_DMAIE_ON      (MPIPE_DMA->CTL |= 0x0004)
#define MPIPE_DMAIE_OFF     (MPIPE_DMA->CTL &= ~0x0004)

#define MPIPE_DMA_TXTRIGGER()   do { \
                                    UART_CLEAR_TXIFG(); \
                                    UART_SET_TXIFG(); \
                                } while(0)
//#define MPIPE_DMA_TXTRIGGER()   (MPIPE_DMA->CTL |= 1)



#define MPIPE_FOOTERBYTES   4





/** Mpipe Module Data
  * At present this consumes 24 bytes of SRAM.  6 bytes can be freed by
  * removing the callbacks, which rarely (if ever) need to be dynamic.
  */
typedef struct {
#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
        ot_u8           ackbuf[10];
        mpipe_priority  priority;
#   endif
    mpipe_state     state;
    Twobytes        sequence;
    ot_u8*          pktbuf;
    ot_int          pktlen;

#   if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
        void (*sig_rxdone)(ot_int);
        void (*sig_txdone)(ot_int);
        void (*sig_rxdetect)(ot_int);
#   endif
} mpipe_struct;

mpipe_struct mpipe;


void sub_signull(ot_int sigval);
void sub_uart_setup();





/**************
 * Mpipe ISRs *
 **************/

#if (ISR_EMBED(MPIPE) == ENABLED)

#if (CC_SUPPORT == CL430)
#   pragma vector=DMA_VECTOR
#elif (CC_SUPPORT == GCC)
    OT_IRQPRAGMA(DMA_VECTOR)
#elif (CC_SUPPORT == IAR_V5)
#else
#   error "A known compiler has not been defined"
#endif
OT_INTERRUPT void mpipe_dma_isr(void) {
    //MPIPE_DMAEN(OFF); //unnecessary on single transfer mode
#   if (MPIPE_DMANUM == 0)
        if (DMA->IV == 2) mpipe_isr();
#   elif (MPIPE_DMANUM == 1)
        if (DMA->IV == 4) mpipe_isr();
#   elif (MPIPE_DMANUM == 2)
        if (DMA->IV == 6) mpipe_isr();
#   else
#       error "This version of MPIPE requires DMA."
#   endif
    LPM4_EXIT;
}


#if (CC_SUPPORT == CL430)
#   pragma vector=MPIPE_I2C_VECTOR
#elif (CC_SUPPORT == GCC)
    OT_IRQPRAGMA(MPIPE_I2C_VECTOR)
#elif (CC_SUPPORT == IAR_V5)
#else
#   error "A known compiler has not been defined"
#endif
OT_INTERRUPT void mpipe_uart_isr(void) {
    //MPIPE_I2C->IFG = 0;
    if ((mpipe.state == MPIPE_Tx_Wait) || (mpipe.state == MPIPE_TxAck_Wait)) {
        mpipe.state++;
    }
    else {
        mpipe_isr();
    }
    LPM4_EXIT;
}
#endif




/*****************************************
 * Mpipe Subroutines (private functions) *
 *****************************************/

#if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
void sub_signull(ot_int sigval) { }

void mpipe_setsig_txdone(void (*signal)(ot_int)) {
    mpipe.sig_txdone = signal;
}

void mpipe_setsig_rxdone(void (*signal)(ot_int)) {
    mpipe.sig_rxdone = signal;
}

void mpipe_setsig_rxdetect(void (*signal)(ot_int)) {
    mpipe.sig_rxdetect = signal;
}
#endif



void sub_uart_portsetup() {
    // Get write-access to port mapping regs, update, re-lock
    PM->PWD                 = 0x2D52;
    MPIPE_I2C_TXMAP         = MPIPE_I2C_TXSIG;
    MPIPE_I2C_RXMAP         = MPIPE_I2C_RXSIG;
    PM->PWD                 = 0;
    
    MPIPE_I2C->IFG          = 0;
    MPIPE_I2C->IE           = 0;
    MPIPE_I2C_PORT->SEL    |= MPIPE_I2C_PINS;       // Set pins to alternate function
  //MPIPE_I2C_PORT->IE     &= ~MPIPE_I2C_PINS;      // disable any interrupts on TX/RX pins
    MPIPE_I2C_PORT->IFG    &= ~MPIPE_I2C_PINS;      // clear any interrupt flags on TX/RX pins
    MPIPE_I2C_PORT->DDIR   &= ~MPIPE_I2C_PINS;      // Clear direction (set input)
    MPIPE_I2C_PORT->DDIR   |= MPIPE_I2C_TXPIN;      // Set TX to output
  //MPIPE_I2C_PORT->DS     |= MPIPE_I2C_TXPIN;      // Set drive strength to high on TX pin
}







/**************************
 * Public Mpipe Functions *
 **************************/

ot_u8 mpipe_footerbytes() {
    return MPIPE_FOOTERBYTES;
}



ot_int mpipe_init(void* port_id) {
/// 0. "port_id" is unused in this impl, and it may be NULL
/// 1. Set all signal callbacks to NULL, and initialize other variables.
/// 2. Prepare the HW, which in this case is a UART
/// 3. Set default speed, which in this case is 115200 bps

#   if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
        mpipe.sig_rxdone    = &sub_signull;
        mpipe.sig_txdone    = &sub_signull;
        mpipe.sig_rxdetect  = &sub_signull;
#   endif

    //mpipe.sequence.ushort   = 0;          //not actually necessary
#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
    mpipe.priority          = MPIPE_Low;
#   endif
    mpipe.state             = MPIPE_Idle;

    sub_uart_portsetup();
    
    MPIPE_I2C->CTL0 = (UCMST | UCMODE_3 | UCSYNC);
    MPIPE_I2C->CTL1 = (UCSSEL_2 | UCSWRST);
    MPIPE_I2C->BRW  = 7;
    MPIPE_I2C->I2COA= MPIPE_I2C_SELF;
    MPIPE_I2C->I2CSA= MPIPE_I2C_TARGET;

    return 0;
}



void mpipe_wait() {
    while (mpipe.state != MPIPE_Idle) {
        SLEEP_MCU();
    }
}




void mpipe_setspeed(mpipe_speed speed) {
    I2C_CLOSE();
    MPIPE_I2C->IE   = 0;
    MPIPE_I2C->IFG  = 0;
    MPIPE_I2C->BRW  = 7;
}


mpipe_state mpipe_status() {
    return mpipe.state;
}




ot_int mpipe_txndef(ot_u8* data, ot_bool blocking, mpipe_priority data_priority) {
/// Data TX occurs after a CTS detect event.  The DMA is setup beforehand, and
/// it is activated in the CTS ISR or manually (in this function) in the cases
/// where CTS might be locked high (i.e. receiver is always ready, maybe it is
/// full duplex or whatever).
    Twobytes crcval;
    ot_int data_length = data[2] + 6;

#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
        if (data_priority != MPIPE_Ack) {
            if (mpipe.state != MPIPE_Idle) {
                return -1;
            }
            mpipe.priority  = data_priority;
            mpipe.pktbuf    = data;
            mpipe.pktlen    = data_length;
        }
#   else
        if (mpipe.state != MPIPE_Idle) {
            return -1;
        }
        mpipe.pktbuf    = data;
#   endif

    mpipe.state = MPIPE_Tx_Wait;

    MPIPE_DMAEN(OFF);

#if (MCU_FEATURE(MPIPEDMA) == ENABLED)
#   if (MPIPE_DMANUM == 0)
        DMA->CTL0  |= MPIPE_I2C_TXTRIG;
#   elif (MPIPE_DMANUM == 1)
        DMA->CTL0  |= (MPIPE_I2C_TXTRIG << 8);
#   elif (MPIPE_DMANUM == 2)
        DMA->CTL1   = MPIPE_I2C_TXTRIG;
#   else
#       error MPIPE_DMANUM is set to a DMA that does not exist on this device
#   endif

    DMA->CTL4 = (DMA_Options_RMWDisable | DMA_Options_RoundRobinDisable | DMA_Options_ENMIEnable);

    // add sequence id & crc to end of the datastream
    data[data_length++] = mpipe.sequence.ubyte[UPPER];
    data[data_length++] = mpipe.sequence.ubyte[LOWER];
    crcval.ushort       = platform_crc_block(data, data_length);
    data[data_length++] = crcval.ubyte[UPPER];
    data[data_length++] = crcval.ubyte[LOWER];

    MPIPE_DMA_TXCONFIG(data, data_length, ON);
    UART_OPEN();
    MPIPE_DMA_TXTRIGGER();

    if (blocking == True) {
        mpipe_wait();
    }

    return data_length;
#else
#endif
}





ot_int mpipe_rxndef(ot_u8* data, ot_bool blocking, mpipe_priority data_priority) {
    ot_int data_length = 10;

#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
        if (data_priority != MPIPE_Ack) {
            if (mpipe.state != MPIPE_Idle) {
                return -1;
            }
            mpipe.priority  = data_priority;
            mpipe.pktbuf    = data;
            mpipe.pktlen    = 6;
        }
#   else
        if (mpipe.state != MPIPE_Idle) {
            return -1;
        }
        mpipe.pktbuf    = data;
        mpipe.pktlen    = 6;
#   endif

    mpipe.state = MPIPE_Idle;
    MPIPE_DMAEN(OFF);

#if (MCU_FEATURE(MPIPEDMA) == ENABLED)
#   if (MPIPE_DMANUM == 0)
        DMA->CTL0  |= MPIPE_I2C_RXTRIG;
#   elif (MPIPE_DMANUM == 1)
        DMA->CTL0  |= (MPIPE_I2C_RXTRIG << 8);
#   elif (MPIPE_DMANUM == 2)
        DMA->CTL1   = MPIPE_I2C_RXTRIG;
#   else
#       error MPIPE_DMANUM is set to a DMA that does not exist on this device
#   endif

    DMA->CTL4 = (DMA_Options_RMWDisable | DMA_Options_RoundRobinDisable | \
                 DMA_Options_ENMIEnable);

    MPIPE_DMA_RXCONFIG(data, data_length, ON);
    I2C_OPEN();
    I2C_CLEAR_RXIFG();

    return 0;
#else
#endif
}




void sub_rxdone() {
    UART_CLOSE();
    mpipe.state = MPIPE_Idle;
#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
        mpipe.priority = MPIPE_Low;
#   endif
#   if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
        mpipe.sig_rxdone(0);
#   endif
}

void sub_txdone() {
    UART_CLOSE();
    mpipe.sequence.ushort++;    //increment sequence on TX Done
    mpipe.state = MPIPE_Idle;
#   if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
        mpipe.priority = MPIPE_Low;
#   endif
#   if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
        mpipe.sig_txdone(0);
#   endif
}

void mpipe_isr() {
/// MPipe is state-based.  Depending on the MPipe implementation and the HW
/// implementation of the DMA+UART, state transitions may happen differently.
/// <LI> In typical RX, there is a header detection event that sets-up a second
///      RX process for downloading the rest of the packet.  When the DMA is
///      done, the process completes.  </LI>
/// <LI> For TX, there is a wait-state needed while the HW UART finishes
///      sending the DMA buffered data (two bytes). </LI>
/// <LI> If MPipe does not have HW acks, then software can be used to manage
///      Acks.  In this case, a complete TX process also requires RX'ing an
///      Ack, and a complete RX process requires TX'ing an Ack. </LI>

    switch (mpipe.state) {
        case MPIPE_Idle: //note, case doesn't break!
#           if (OT_FEATURE(MPIPE_CALLBACKS) == ENABLED)
                mpipe.sig_rxdetect(0);
#           endif

        case MPIPE_RxHeader: {
            mpipe.state     = MPIPE_RxPayload;
            mpipe.pktlen   += mpipe.pktbuf[2] + MPIPE_FOOTERBYTES;
            //@todo Might need to put in a DMA Disabler here
            MPIPE_DMA_RXCONFIG(mpipe.pktbuf+6, mpipe.pktlen-6, ON);
            break;
        }

        case MPIPE_RxPayload:
#           if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
            if (mpipe.priority != MPIPE_Broadcast) {
                // Copy RX'ed seq into ACK
                ot_u8*  seq_val             = &mpipe.pktbuf[mpipe.pktlen-MPIPE_FOOTERBYTES];
                mpipe.sequence.ubyte[UPPER] = *seq_val++;
                mpipe.sequence.ubyte[LOWER] = *seq_val;

                mpipe.ackbuf[0] = 0xDD;     //NDEF message flags
                mpipe.ackbuf[1] = 0;        //Typelen = 0
                mpipe.ackbuf[2] = 0;        //Paylod len = 0
                mpipe.ackbuf[3] = 2;        //ID len = 2
                mpipe.ackbuf[4] = 0;
                mpipe.ackbuf[5] = (platform_crc_block(mpipe.pktbuf, mpipe.pktlen)) ? 0x7F : 0;

                mpipe_txndef(mpipe.ackbuf, False, MPIPE_Ack);
                mpipe.state = MPIPE_TxAck_Wait;
                break;
            }
#           endif
            // Broadcast, so no ACK
            sub_rxdone();
            break;

        case MPIPE_TxAck_Wait:
            MPIPE_I2C->IE = UCTXIE;
            break;

        case MPIPE_TxAck_Done:  // TX'ed an ACK
            MPIPE_I2C->IE = 0;
#           if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
            if (mpipe.ackbuf[5] != 0) { // TX'ed a NACK
                mpipe_rxndef(mpipe.pktbuf, False, mpipe.priority);
                mpipe.state = MPIPE_RxHeader;
                break;
            }
#           endif
            sub_rxdone();
            break;

        case MPIPE_Tx_Wait:
            MPIPE_I2C->IE = UCTXIE;
            break;

        case MPIPE_Tx_Done:
            MPIPE_I2C->IE = 0;
#           if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
            if (mpipe.priority != MPIPE_Broadcast) {
                mpipe_rxndef(mpipe.ackbuf, False, MPIPE_Ack);
                mpipe.state = MPIPE_RxAck;
                break;
            }
#           endif
            sub_txdone();  // Broadcast, so no ACK
            break;

        case MPIPE_RxAck:
#           if (PLATFORM_FEATURE_USBCONVERTER != ENABLED)
            if (platform_crc_block(mpipe.ackbuf, 10) != 0) { //RX'ed NACK
                mpipe_txndef(mpipe.pktbuf, False, mpipe.priority);
                break;
            }
#           endif
            //RX'ed ACK
            sub_txdone();
            break;
    }
}


#endif

