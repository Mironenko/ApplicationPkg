/** @file

  Copyright (c) 2008 - 2011, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "SmartCard.h"
#include "Apdu.h"
#include <Library/TimerLib.h>
#include <Library/HiiLib.h>
#include "eTokenVfrData.h"
#include <Protocol/HiiConfigAccess.h>
#include <Protocol/HiiString.h>
#include <Guid/MdeModuleHii.h>
#include "eToken.h"


#define USB_REQUEST_TYPE(Dir, Type, Target) \
          ((UINT8)((((Dir) == EfiUsbDataIn ? 0x01 : 0) << 7) | (Type) | (Target)))

#define CCID_DEFAULT_TIMEOUT      10000
#define ATR_DEFAULT_LEN           19
#define DEFAULT_RESPONSE_LEN      64 // XXX it must be low then MAX_PACKET_SIZE


//#define ETOKEN_PROD_ID            0x0620
//#define ETOKEN_VENDOR_ID          0x0529
#if 1
typedef struct _PRIVATE_DATA {
  EFI_HII_CONFIG_ACCESS_PROTOCOL   ConfigAccess;
  EFI_HII_DATABASE_PROTOCOL       *HiiDatabase;
  EFI_HII_STRING_PROTOCOL         *HiiString;
  EFI_HANDLE                      HiiHandle;
} PRIVATE_DATA;

STATIC PRIVATE_DATA gTokenPrivateData;
STATIC VOID *StartOpCodeHandle, *EndOpCodeHandle;
STATIC UINT8 eTokenModeOpt[2];
#endif


STATIC
EFI_STATUS
GeteTokenScMode (
  IN OUT UINT8 *Mode
  )
{
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  EFI_GUID VarGuid = ETOKEN_SETUP_VAR_GUID;
  UINTN Size;
  ETOKEN_VARSTORE_DATA Data;
  
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  Size = sizeof (ETOKEN_VARSTORE_DATA);
  ZeroMem (&Data, sizeof (Data));
  Status = gRT->GetVariable (
              ETOKEN_VAR_NAME, 
              &VarGuid, 
              NULL, 
              &Size, 
              &Data);
  if (!EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    *Mode = Data.eTokenMode;
  }
  DEBUG((EFI_D_ERROR, "%a.%d Status=%X\n", 
    __FUNCTION__, __LINE__, Status));
  return Status;
}


EFI_STATUS
GetCcidDescriptor(
  IN EFI_USB_IO_PROTOCOL *UsbIo,
  IN UINT16 Interface,
  OUT USB_CCID_DESCRIPTOR* Descriptor
  )

{
  EFI_STATUS Status;
  EFI_USB_DEVICE_REQUEST Request;
  UINT16 Length;
  UINT32 UsbStatus;
  DEBUG((EFI_D_ERROR, "%s: Interface:%d\n", L"GetCcidDescriptor", Interface ));

  Length = sizeof(USB_CCID_DESCRIPTOR);

  Request.RequestType = USB_REQUEST_TYPE(EfiUsbDataIn, USB_REQ_TYPE_STANDARD,
                                         USB_TARGET_DEVICE);
  Request.Request = USB_REQ_GET_DESCRIPTOR;
  Request.Value = 0x21 << 8; // 0x21 FUNCTIONAL DESCRIPTOR
  Request.Index = Interface;
  Request.Length = Length;

  DEBUG((EFI_D_ERROR, "%a.%d --> UsbControlTransfer\n", __FUNCTION__, __LINE__));
  
  Status = UsbIo->UsbControlTransfer( UsbIo,
      &Request,
      EfiUsbDataIn,
      CCID_DEFAULT_TIMEOUT,
      Descriptor,
      Length,
      &UsbStatus 
      );

  if(EFI_ERROR(Status) && UsbStatus != EFI_USB_NOERROR) {
    Status = EFI_DEVICE_ERROR;
  }

  EFI_ERROR_RET(Status, "Read CCID descriptor Error");

  DEBUG((EFI_D_ERROR, "Version:%x\n", Descriptor->bcdCCID));
  DEBUG((EFI_D_ERROR, "Protocols:%x\n", Descriptor->dwProtocols));
  DEBUG((EFI_D_ERROR, "Voltage support:%x\n", Descriptor->bVoltageSupport));
  DEBUG((EFI_D_ERROR, "Features:%x\n", Descriptor->dwFeatures));

OnError:

  return Status;
}


EFI_STATUS
EFIAPI
eTokenCcidDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
eTokenCcidDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  );

EFI_STATUS
EFIAPI
eTokenCcidDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN  EFI_HANDLE                     Controller,
  IN  UINTN                          NumberOfChildren,
  IN  EFI_HANDLE                     *ChildHandleBuffer
  );

EFI_DRIVER_BINDING_PROTOCOL gUsbCcidDriverBinding = {
  eTokenCcidDriverBindingSupported,
  eTokenCcidDriverBindingStart,
  eTokenCcidDriverBindingStop,
  0xa,
  NULL,
  NULL
};

PROTOCOL_NUMBER
DriverToProtocolType(
  IN CCID_PROTOCOL Protocol
  )
{
  if(Protocol == CCID_PROTOCOL_T1) {
    return PROTOCOL_T1;
  } else {
    return PROTOCOL_T0;
  }
}

CCID_PROTOCOL
ProtocolTypeToDriver(
  IN PROTOCOL_NUMBER Protocol
  )
{
  if(Protocol == PROTOCOL_T1) {
    return CCID_PROTOCOL_T1;
  } else {
    return CCID_PROTOCOL_T0;
  }
}


EFI_STATUS
CcidSendCommand(
  IN     USB_CCID_DEV *Device,
  IN     CCID_CMD *Command,
  IN OUT CCID_RESPONSE_MSG_HEADER *Result,
  IN OUT UINTN *ResultLength
  )
{
  EFI_STATUS Status;
  EFI_USB_IO_PROTOCOL *UsbIo;
  UINT32 UsbStatus;
  UINTN Length;

  if (Command == NULL || Device == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Status = EfiAcquireLockOrFail(&Device->UsbCcidLock);
  if (EFI_ERROR(Status)) {
    return EFI_DEVICE_ERROR;
  }  

  UsbIo = Device->UsbIo;

  Length = sizeof(CCID_CMD_HEADER) + sizeof(CCID_CTL) + Command->Header.Length;
#if 0  
  DumpBytes((UINT8*)Command, Length);
#endif
  Status = UsbIo->UsbBulkTransfer(
          UsbIo,
          Device->OutEndpointDescriptor.EndpointAddress,
          Command,
          &Length,
          CCID_DEFAULT_TIMEOUT,
          &UsbStatus
          );

  EFI_ERROR_RET(Status, "");
  Status = CHECK_USB_STATUS(UsbStatus);
  EFI_ERROR_RET(Status, "");

  while (TRUE) {
    Length = *ResultLength;

    Status = UsbIo->UsbBulkTransfer(
                  UsbIo,
                  Device->InEndpointDescriptor.EndpointAddress,
                  Result,
                  &Length,
                  CCID_DEFAULT_TIMEOUT,
                  &UsbStatus
                  );

    EFI_ERROR_RET(Status, "");
    Status = CHECK_USB_STATUS(UsbStatus);
    EFI_ERROR_RET(Status, "");

#if 0
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));  
    DumpBytes((UINT8*)Result, Length);
#endif

    if (Command->Header.Slot == Result->Slot && 
        Command->Header.Sequence == Result->Sequence) {
      Status = CcidCheckRespone( Result, Length );
      if (Status == EFI_SUCCESS) {
        break;
      }
      if (Status == EFI_NOT_READY) {
        continue;
      }
      EFI_ERROR_RET(Status, "");
    }
  }

  Status = EFI_SUCCESS;
  *ResultLength = Length;

OnError:
  if (Device->bDriverStopped) {
    FreePool(Device);
  }
  EfiReleaseLock(&Device->UsbCcidLock);
  return Status;
}

EFI_STATUS
EFIAPI
SmartCardReaderInit (
  IN SMART_CARD_READER_PROTOCOL *This,
  IN VOID **Param
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderFinish (
  IN SMART_CARD_READER_PROTOCOL *This
  )
{
  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
SmartCardReaderRelease (
  IN SMART_CARD_READER_PROTOCOL *This
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderConnect (
  IN SMART_CARD_READER_PROTOCOL *This,
  IN SMART_CARD_READER_SLOT_INFO *Slot
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderDisconnect (
  IN SMART_CARD_READER_PROTOCOL *This,
  IN SMART_CARD_READER_SLOT_INFO *Slot
  )
{
  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
CcidHandler(
  IN VOID         *Data,
  IN UINTN        DataLength,
  IN VOID         *Context,
  IN UINT32       Status
  )
{
  return EFI_SUCCESS;
}

VOID
DumpResponse(
  IN CCID_RESPONSE_MESSAGE *Response
  )
{
  UINTN Index = 0;
  if(Response == NULL) {
    DEBUG(( EFI_D_ERROR, "Error: Response == NULL\n" ));
    return;
  }

  DEBUG(( EFI_D_ERROR, "Response\n" ));
  DEBUG(( EFI_D_ERROR, "  Command:%02x\n", Response->Header.Command ));
  DEBUG(( EFI_D_ERROR, "  Length:%02d\n", Response->Header.Length ));
  DEBUG(( EFI_D_ERROR, "  Seq:%02d\n", Response->Header.Sequence ));
  DEBUG(( EFI_D_ERROR, "  Slot:%02d\n", Response->Header.Slot ));
  DEBUG(( EFI_D_ERROR, "  bError:%02d\n", Response->Header.bError ));
  DEBUG(( EFI_D_ERROR, "  bStatus.bmCommandStatus:%02d\n", Response->Header.bStatus.bmCommandStatus ));
  DEBUG(( EFI_D_ERROR, "  bStatus.bmICCStatus:%02d\n", Response->Header.bStatus.bmICCStatus ));

  if( Response->Header.Length ) {
    DEBUG(( EFI_D_ERROR, "  Data:" ));
    for( Index = 0; Index < Response->Header.Length; Index++ ) {
      if(!(Index & (~(UINTN) 0x0F))) {
        DEBUG(( EFI_D_ERROR, "\n" ));
      }
      DEBUG(( EFI_D_ERROR, "%02x ", Response->RawData[Index]));
    }
    DEBUG(( EFI_D_ERROR, "\n" ));
  }
}

EFI_STATUS
SmartCardReaderGetStatus(
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8 Slot,
  IN     UINT8 *ScStatus
  )
{
  EFI_USB_IO_PROTOCOL *UsbIo;
  USB_CCID_DEV *CcidDev;
  EFI_STATUS Status;
  CCID_CMD Cmd;
  CCID_RESPONSE_MESSAGE Response;
  UINTN Length;

  CcidDev = CR(This, USB_CCID_DEV, SmartCardReaderIo, CCID_DEV_SIGNATURE);
  ASSERT(CcidDev);
  UsbIo = CcidDev->UsbIo;
  DEBUG((EFI_D_ERROR, "SmartCardReaderGetStatus \n"));


  Status = CcidBuildCommand(&Cmd,
                            sizeof(Cmd),
                            Slot,
                            CCID_CMD_GETSLOTSTAT,
                            NULL,
                            NULL,
                            0,
                            &CcidDev->Sequence
                            );

  EFI_ERROR_RET(Status, "");

  Length = sizeof(Response.Header) + sizeof(CCID_RESPONSE_SLOT_STATUS);
  Status = CcidSendCommand( CcidDev, &Cmd, &Response.Header, &Length );

  EFI_ERROR_RET(Status, "");

  DumpResponse(&Response);

  *ScStatus = Response.Header.bStatus.bmICCStatus != CCID_STATUS_ICC_NOT_PRESENT ? 1 : 0;
//  CcidDev->Status[slot] = *ScStatus;
  return EFI_SUCCESS;
OnError:
  return Status;
}


EFI_STATUS
CcidSetProtocol(
  IN     USB_CCID_DEV *Device,
  IN     UINT8 Slot,
  IN     CCID_PROTOCOL Protocol
  )
{
  EFI_STATUS Status;
  CCID_CMD Cmd;
  CCID_RESPONSE_MESSAGE Response;
  //UINTN ResponseLength;
  CCID_RESPONSE_MESSAGE TmpResponse;
  UINTN TmpLength;
  CCID_ATR_INFO *AtrInfo;
  UINT8 *Params = NULL;
  UINTN ParamLength = 0;
  PROTOCOL_NUMBER ParamProtocol = PROTOCOL_T0;

  Status = EFI_SUCCESS;

  if( (Device->Flags & FLAG_PROTOCOL_ESCAPE) &&
       Protocol != CCID_PROTOCOL_ESCAPE &&
       Slot == (Device->Slots - 1)) {
    Status = EFI_UNSUPPORTED;
    EFI_ERROR_RET(Status, "");
  }

  switch(Protocol) {
  case CCID_PROTOCOL_T0:
    if( !(Device->Flags & FLAG_PROTOCOL_T0) ) {
      Status = EFI_UNSUPPORTED;
    }
    break;

  case CCID_PROTOCOL_T1:
    if( !(Device->Flags & FLAG_PROTOCOL_T1) ) {
      Status = EFI_UNSUPPORTED;
    }
    break;

  case CCID_PROTOCOL_ESCAPE:
    if( !(Device->Flags & FLAG_PROTOCOL_ESCAPE) ) {
      Status = EFI_UNSUPPORTED;
    }
    break;

  default:
    Status = EFI_UNSUPPORTED;
    break;
  }

  EFI_ERROR_RET(Status, "Protocol unsupported");

  if( Device->ReaderType == TYPE_APDU ) {
    Device->State[Slot].Protocol = Protocol;
    return EFI_SUCCESS;
  }

  AtrInfo = &Device->State[Slot].Atr;

  CcidParseAtr( AtrInfo, Device->State[Slot].AtrBlob, Device->State[Slot].AtrBlobLength );

  if( !(Device->Flags & FLAG_NO_SETPARAM) ) {
    UINTN Length;
    Status = CcidBuildCommand(
                &Cmd,
                sizeof(Cmd),
                Slot,
                CCID_CMD_GETPARAMS,
                NULL,
                NULL,
                0,
                &Device->Sequence
                );

    EFI_ERROR_RET(Status, "");

    Length = sizeof(Response);
    Status = CcidSendCommand( Device, &Cmd, &Response.Header, &Length );

    EFI_ERROR_RET(Status, "");

    Params = (UINT8*) &Response.Params;


    if( Protocol == CCID_PROTOCOL_T0 ) {
      ParamLength =  sizeof(Response.Params.T0);
      ParamProtocol = PROTOCOL_T0;
    } else {
      ParamLength =  sizeof(Response.Params.T1);
      ParamProtocol = PROTOCOL_T1;
    }

    if( !(Device->Flags & (FLAG_NO_PTS | FLAG_AUTO_ATRPARSE))
        && (AtrInfo->TC[0] != ATR_NONE) ) {

      Status = CcidBuildCommand(
                      &Cmd,
                      sizeof(Cmd),
                      Slot,
                      CCID_CMD_SETPARAMS,
                      NULL,
                      Params,
                      ParamLength,
                      &Device->Sequence
                      );

    EFI_ERROR_RET(Status, "");

    Cmd.Ctl.SetParameters.bProtocolNum = ParamProtocol;
    TmpLength = sizeof(TmpResponse);

    Status = CcidSendCommand( Device, &Cmd, &TmpResponse.Header, &TmpLength );

    EFI_ERROR_RET(Status, "");

    }

  }

  if( !(Device->Flags & FLAG_NO_PTS) &&
       (Protocol == CCID_PROTOCOL_T1 || AtrInfo->TA[0] != ATR_NONE )) {
    PPS Pps;
    UINT8 *Data;
    UINT8 Pck;
    UINTN PpsLength;
    //CCID_RESPONSE_MESSAGE PpsResponse;
    //UINTN PpsResponseLength;
    UINTN Index,i;

    Index = PPS1_INDEX;
    ZeroMem( &Pps, sizeof(PPS));
    Pps.PPSS = 0xFF;
    Pps.PPS0.T = DriverToProtocolType(Protocol);

    if( AtrInfo->TA[0] != ATR_NONE ) {
      Pps.Optional[Index++] = AtrInfo->TA[0];
      Pps.PPS0.PPS1_Present = TRUE;
    }

    if( AtrInfo->TC[0] != ATR_NONE ) {
      Pps.Optional[Index++] = AtrInfo->TC[0];
      Pps.PPS0.PPS2_Present = TRUE;
    }

    Data =  &Pps.PPS0_Raw;
    for (i = 0, Data = &Pps.PPS0_Raw, Pck = 0; &Data[i] < &Pps.Optional[Index]; i++) {
      Pck ^= Data[i];
    }

    Pps.Optional[Index] = Pck;


    PpsLength = FIELD_OFFSET (PPS, Optional[Index + 1]);

    Status = CcidBuildCommand(
                &Cmd,
                sizeof(Cmd),
                Slot,
                CCID_CMD_XFRBLOCK,
                NULL,
                &Pps.PPS0_Raw,
                PpsLength,
                &Device->Sequence
                );

    EFI_ERROR_RET(Status, "");

    TmpLength = sizeof(TmpResponse);

    if( Device->ReaderType == TYPE_CHAR) {
    Cmd.Ctl.XfrBlock.wLevelParameter = (UINT16) TmpLength;
    }

    Status = CcidSendCommand( Device, &Cmd, &TmpResponse.Header, &TmpLength );

    EFI_ERROR_RET(Status, "");
    // TODO Check PPS response
  }

  if( !(Device->Flags & FLAG_NO_SETPARAM) && (!(Device->Flags
        & FLAG_NO_SETPARAM) || (Protocol != CCID_PROTOCOL_T0)) ) {

    if( !(Device->Flags & FLAG_AUTO_ATRPARSE) ) {
      switch( Protocol ) {
      case CCID_PROTOCOL_T0:
        if( AtrInfo->TA[0] != ATR_NONE ) {
          Response.Params.T0.bmFindexDindex = AtrInfo->TA[0];
        }
        if( AtrInfo->TC[0] != ATR_NONE ) {
          Response.Params.T0.bGuardTimeT0 = AtrInfo->TC[0];
        }
        if( AtrInfo->TC[1] != ATR_NONE ) {
          Response.Params.T0.bWaitingIntegersT0 = AtrInfo->TC[1];
        }
        // XXX check for clock stop support
        if( AtrInfo->TA[2] != ATR_NONE ) {
          Response.Params.T0.bClockStop = AtrInfo->TA[2] >> 6;
        }
        break;
        
      case CCID_PROTOCOL_T1:
        if( AtrInfo->TA[0] != ATR_NONE ) {
          Response.Params.T1.bmFindexDindex = AtrInfo->TA[0];
        }
        Response.Params.T1.bmTCCKST1.ProtocolSpecific = 4;
        Response.Params.T1.bmTCCKST1.Convertion = 0; // XXX
        if( AtrInfo->TC[2] == 1 ) {
          Response.Params.T1.bmTCCKST1.CheckSumType = PARAMS_T1_CS_CRC;
        } else {
          Response.Params.T1.bmTCCKST1.CheckSumType = PARAMS_T1_CS_LRC;
        }
        if( AtrInfo->TC[0] != ATR_NONE ) {
          Response.Params.T1.bGuardTimeT1 = AtrInfo->TC[0];
        }
        if( AtrInfo->TB[2] != ATR_NONE ) {
          Response.Params.T1.bWaitingIntegersT1 = AtrInfo->TB[2];
        }
       /*
             * XXX CCID supports setting up clock stop for T=1, but the
             * T=1 ATR does not define a clock-stop byte.
             */
        if( AtrInfo->TA[2] != ATR_NONE ) {
          Response.Params.T1.bIFCS = AtrInfo->TA[2];
        }

        break;
        
      default:
        break;
      }
    }

    Status = CcidBuildCommand(
              &Cmd,
              sizeof(Cmd),
              Slot,
              CCID_CMD_SETPARAMS,
              NULL,
              Params,
              ParamLength,
              &Device->Sequence
              );

    Cmd.Ctl.SetParameters.bProtocolNum = ParamProtocol;

    EFI_ERROR_RET(Status, "");

    TmpLength = sizeof(TmpResponse);
    Status = CcidSendCommand( Device, &Cmd, &TmpResponse.Header, &TmpLength );

    EFI_ERROR_RET(Status, "");
  }

  return EFI_SUCCESS;

OnError:
  return Status;
}

EFI_STATUS
SmartCardReaderReset(
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8  Slot,
  IN OUT UINT8* Atr,
  IN OUT UINTN* AtrLength
  )
{
  EFI_USB_IO_PROTOCOL *UsbIo;
  USB_CCID_DEV *CcidDev;
  EFI_STATUS Status;
  UINT8 IccStatus;
  CCID_CMD Cmd;
  CCID_RESPONSE_MESSAGE Response;
  UINTN ResponseLength;
//  UINT8 Index;
/*
  UINT8 Voltage[] = {
    FLAG_VOLTAGE_AUTO, // 0 - Auto
    FLAG_VOLTAGE_50V,  // 1 - 5.0
    FLAG_VOLTAGE_33V,  // 2 - 3.3
    FLAG_VOLTAGE_18V,  // 3 - 1.8
    0
    };
*/
//  UINT8 ScStatus;

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));

  CcidDev = CR(This, USB_CCID_DEV, SmartCardReaderIo, CCID_DEV_SIGNATURE);
  ASSERT(CcidDev);
  UsbIo = CcidDev->UsbIo;
  DEBUG((EFI_D_ERROR, "SmartCardReaderReset \n"));

  if (Atr == NULL || AtrLength == NULL || This == NULL ) {
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    return EFI_INVALID_PARAMETER;
  }
#if 0
  Status = SmartCardReaderGetStatus( This, Slot, &IccStatus );

  DEBUG((EFI_D_ERROR, "%a.%d Status=%08X\n", __FUNCTION__, __LINE__, Status));
#else
  (VOID)IccStatus;
#endif
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  Status = CcidBuildCommand(
                      &Cmd,
                      sizeof(Cmd),
                      Slot,
                      CCID_CMD_ICCPOWEROFF,
                      NULL, // ctl
                      NULL, // send data
                      0,
                      &CcidDev->Sequence
                      );
  ResponseLength = *AtrLength;
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  Status = CcidSendCommand( CcidDev, &Cmd, &Response.Header, &ResponseLength );
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));  

  if( (CcidDev->Flags & FLAG_PROTOCOL_ESCAPE) && (Slot == (CcidDev->Slots - 1))) {
    CcidSetProtocol( CcidDev, Slot, CCID_PROTOCOL_ESCAPE );
    Atr[0] = 0xFF;
    *AtrLength = 1;
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    return EFI_SUCCESS;
  }

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  
  Status = CcidBuildCommand(
                &Cmd,
                sizeof(Cmd),
                Slot,
                CCID_CMD_ICCPOWERON,
                NULL, // ctl
                NULL, // send data
                0,
                &CcidDev->Sequence
                );

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));

  ResponseLength = *AtrLength;

  Status = CcidSendCommand( CcidDev, &Cmd, &Response.Header, &ResponseLength );
  // TODO Check response

  if (EFI_ERROR(Status) ) {
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    return Status;
  }
  if( Response.Header.Length > *AtrLength ) {
    Status = EFI_BUFFER_TOO_SMALL;
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    return Status;
  }
  
  CopyMem(Atr, &Response.RawData[sizeof(CCID_RDR_TO_PC_DATA_BLOCK)], 
    Response.Header.Length );
        
  *AtrLength = Response.Header.Length;

  Status = EFI_SUCCESS;
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  return Status;
}

EFI_STATUS
EFIAPI
SmartCardReaderSend(
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8 Slot,
  IN     UINT8 *SendBuffer,
  IN     UINTN SendSize,
  OUT    UINT8 *ScStatus
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderReceive(
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8 Slot,
  IN     UINT8 *RecvBuffer,
  IN     UINTN *RecvSize,
  IN     UINTN Timeout,
  OUT    UINT8 *ScStatus
  )
{
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderSendTpdu(
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8 Slot,
  IN     APDU* Tpdu
  )
{
  DEBUG((EFI_D_ERROR, "SmartCardReaderSendTpdu\n"));
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartCardReaderTransmit (
  IN     SMART_CARD_READER_PROTOCOL *This,
  IN     UINT8 *SendBuffer,
  IN     UINTN SendSize,
  IN OUT UINT8 *RecvBuffer,
  IN OUT UINTN *RecvSize,
  IN     UINT32 Control
  )
{
  USB_CCID_DEV *CcidDev;
  EFI_STATUS Status;
  CCID_CMD Cmd;
  CCID_RESPONSE_MESSAGE Response;
  UINTN ResponseLength;
  UINTN Slot = 0;
  
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));

  ASSERT(This != NULL);
  CcidDev = CR(This, USB_CCID_DEV, SmartCardReaderIo, CCID_DEV_SIGNATURE);
  ASSERT(CcidDev);

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  DumpBytes(SendBuffer, SendSize);

  if( SendBuffer == NULL || SendSize == 0 ) {
    if( (CcidDev->ReaderType != TYPE_APDU && 
         CcidDev->ReaderType != TYPE_TPDU) ||
         CcidDev->State[Slot].Protocol != CCID_PROTOCOL_T0) {
      Status = EFI_UNSUPPORTED;
      DEBUG((EFI_D_ERROR, "%a.%d Error!\n", __FUNCTION__, __LINE__));
      goto _exit;
    }
  }

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));

  Status = CcidBuildCommand(
              &Cmd,
              sizeof(Cmd),
              0, // Slot XXX
              CCID_CMD_XFRBLOCK,
              NULL,
              SendBuffer,
              SendSize,
              &CcidDev->Sequence
              );

  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    goto _exit;
  }

  ResponseLength = sizeof( CCID_RESPONSE_MSG_HEADER ) +
                   sizeof(CCID_RDR_TO_PC_DATA_BLOCK) +
                   ((RecvSize != NULL) ? *RecvSize : 0 );

  if(ResponseLength > sizeof( Response)) {
    Status = EFI_BAD_BUFFER_SIZE;
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    goto _exit;
  }

  Status = CcidSendCommand( CcidDev, &Cmd, &Response.Header, &ResponseLength );

#if 0
  DEBUG((EFI_D_ERROR, "SmartCardReaderTransmit ResponseLength:%d Response.Header.Length:%d\n",
    ResponseLength,
    Response.Header.Length
    ));
#endif

  
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    goto _exit;
  }
  

  ASSERT( ResponseLength >= (sizeof(CCID_RESPONSE_MSG_HEADER) + sizeof(CCID_RDR_TO_PC_DATA_BLOCK)));

  ResponseLength -= sizeof( CCID_RESPONSE_MSG_HEADER ) + sizeof(CCID_RDR_TO_PC_DATA_BLOCK);

  if ( ResponseLength > *RecvSize ) {
    Status = EFI_BUFFER_TOO_SMALL;
    DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    goto _exit;
  }

  CopyMem( RecvBuffer, &Response.RawData[sizeof(CCID_RDR_TO_PC_DATA_BLOCK)], ResponseLength );
  *RecvSize = ResponseLength;

  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));

  return EFI_SUCCESS;
_exit:
  DEBUG((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  return Status;
}


/**
  Uses USB I/O to check whether the device is a USB Smartcard device.

  @param  UsbIo    Pointer to a USB I/O protocol instance.

  @retval TRUE     Device is a USB CCID device.
  @retval FALSE    Device is a not USB CCID device.

**/
BOOLEAN
EFIAPI
IseToken (
  IN  EFI_USB_IO_PROTOCOL       *UsbIo
  )
{
  EFI_STATUS                    Status;
  EFI_USB_INTERFACE_DESCRIPTOR  InterfaceDescriptor;

  //
  // Get the default interface descriptor
  //
  Status = UsbIo->UsbGetInterfaceDescriptor (
                    UsbIo,
                    &InterfaceDescriptor
                    );

  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if (InterfaceDescriptor.InterfaceClass == CLASS_SMARTCARD &&
      InterfaceDescriptor.InterfaceSubClass == 0 &&
      InterfaceDescriptor.InterfaceProtocol == PROTOCOL_CCID
      ) {
    EFI_USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    
    Status = UsbIo->UsbGetDeviceDescriptor(
          UsbIo,
          &DeviceDescriptor
          );
  
    DEBUG((EFI_D_ERROR, "%a.%d Status=%r\n", __FUNCTION__, __LINE__, Status));
    if (EFI_ERROR (Status)) {
      return FALSE;
    }

    DEBUG((EFI_D_ERROR, "\nIdProduct=0x%X IdVendor=0x%X\n", 
      DeviceDescriptor.IdProduct, DeviceDescriptor.IdVendor));

    if (DeviceDescriptor.IdProduct == ETOKEN_PROD_ID &&
        DeviceDescriptor.IdVendor == ETOKEN_VENDOR_ID) {
      DEBUG((EFI_D_ERROR, "%a.%d It is eToken!\n",
        __FUNCTION__, __LINE__));
      return TRUE;
    }
  }
  return FALSE;
}


/**
  Check whether CCID driver supports this device.

  @param  This                   The USB keyboard driver binding protocol.
  @param  Controller             The controller handle to check.
  @param  RemainingDevicePath    The remaining device path.

  @retval EFI_SUCCESS            The driver supports this controller.
  @retval other                  This device isn't supported.

**/
EFI_STATUS
EFIAPI
eTokenCcidDriverBindingSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  )
{
  EFI_STATUS          Status;
  EFI_USB_IO_PROTOCOL *UsbIo;

  //
  // Check if USB I/O Protocol is attached on the controller handle.
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **) &UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (EFI_ERROR (Status)) {
    //  DEBUG((EFI_D_ERROR, "SC: OpenProtocol %r\n", Status));
    return Status;
  }

  //
  // Use the USB I/O Protocol interface to check whether Controller is
  // a CCID device that can be managed by this driver.
  //
  Status = EFI_SUCCESS;

  if (!IseToken (UsbIo)) {
    Status = EFI_UNSUPPORTED;
  }

  gBS->CloseProtocol (
         Controller,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  return Status;
}

STATIC
EFI_STATUS
EFIAPI
FakeExtractConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL   *This,
  IN  CONST EFI_STRING                       Request,
  OUT EFI_STRING                             *Progress,
  OUT EFI_STRING                             *Results
  )
{
  if (Progress == NULL || Results == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  
  *Progress = Request;
  return EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
EFIAPI
FakeRouteConfig (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL   *This,
  IN  CONST EFI_STRING                       Configuration,
  OUT EFI_STRING                             *Progress
  )
{
  if (Configuration == NULL || Progress == NULL) {
    return EFI_INVALID_PARAMETER;
  }
#if 0
  *Progress = Configuration;
  *Progress = Configuration + StrLen (Configuration);
#endif  
  return EFI_SUCCESS;
}

#if 1
STATIC
VOID
DestroyHiiResources(
  VOID
  )
{
  if (StartOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle (StartOpCodeHandle);
    StartOpCodeHandle = NULL;
  }
  if (EndOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle (EndOpCodeHandle);
    EndOpCodeHandle = NULL;
  }
}

STATIC
EFI_STATUS
AllocateHiiResources(
  IN EFI_HII_HANDLE HiiHandle
  )
{
  EFI_IFR_GUID_LABEL *StartLabel;
  EFI_IFR_GUID_LABEL *EndLabel;
  EFI_GUID FormSetGuid = ETOKEN_VFR_GUID;
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  EFI_FORM_ID FormId = 0x1000;
  
  DestroyHiiResources();
  
  StartOpCodeHandle = HiiAllocateOpCodeHandle ();
  if (StartOpCodeHandle == NULL) {
    goto _exit;
  }

  EndOpCodeHandle = HiiAllocateOpCodeHandle ();
  if (EndOpCodeHandle == NULL) {
    goto _exit;
  }

  StartLabel = (EFI_IFR_GUID_LABEL *) HiiCreateGuidOpCode (
      StartOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));

  StartLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  StartLabel->Number       = START_LABEL_ID;

  EndLabel = (EFI_IFR_GUID_LABEL *) HiiCreateGuidOpCode (
      EndOpCodeHandle, &gEfiIfrTianoGuid, NULL, sizeof (EFI_IFR_GUID_LABEL));

  EndLabel->ExtendOpCode = EFI_IFR_EXTEND_OP_LABEL;
  EndLabel->Number       = END_LABEL_ID;
  
  Status = EFI_SUCCESS;
  
  HiiUpdateForm(HiiHandle, &FormSetGuid, FormId,
      StartOpCodeHandle, EndOpCodeHandle);
  
_exit:  
  return Status;
}

STATIC 
EFI_STATUS
eTokenModeOneOfString(
  IN EFI_HII_HANDLE HiiHandle,
  IN VOID *StartOpCodeHandle,
  IN VOID *EndOpCodeHandle,
  IN EFI_STRING_ID Caption,
  IN EFI_QUESTION_ID QuestionId
  )
{
  VOID *OptionsOpCodeHandle = NULL, *Opt1, *Opt2;
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  EFI_STRING_ID Str16_1, Str16_2;
  EFI_GUID VarGuid = ETOKEN_SETUP_VAR_GUID;
  UINTN Size;
  ETOKEN_VARSTORE_DATA Data;
  EFI_GUID FormSetGuid = ETOKEN_VFR_GUID;
  
  DEBUG((EFI_D_ERROR, "%a.%d QuestionId=0x%X\n", 
    __FUNCTION__, __LINE__, QuestionId));
  Size = 0;
  ZeroMem (&Data, sizeof (Data));
  Status = gRT->GetVariable (ETOKEN_VAR_NAME, &VarGuid, NULL, &Size, NULL);
  if (Status == EFI_NOT_FOUND) {
    DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    Data.eTokenMode = SC_MODE_GOST;
    Status = gRT->SetVariable (
              ETOKEN_VAR_NAME, 
              &VarGuid,
              EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
              sizeof (Data),
              &Data);
    if (EFI_ERROR(Status)) {
      return Status;
    }
  } else if (Status != EFI_BUFFER_TOO_SMALL) {
    return Status;
  } else {
    Status = gRT->GetVariable (ETOKEN_VAR_NAME, &VarGuid, NULL, &Size, &Data);
    if (EFI_ERROR(Status)) {
      DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
      return Status;
    }
  }

  OptionsOpCodeHandle = HiiAllocateOpCodeHandle ();
  if (OptionsOpCodeHandle == NULL) {
    DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    return EFI_OUT_OF_RESOURCES;
  }
  
  ZeroMem(eTokenModeOpt, sizeof(eTokenModeOpt));
  if (Data.eTokenMode == SC_MODE_GOST) {
    eTokenModeOpt[SC_MODE_PRO] = 1;
    Str16_1 = STRING_TOKEN (STR_ONE_OF_MODE_GOST);
    Str16_2 = STRING_TOKEN (STR_ONE_OF_MODE_ETOKEN_PRO);
  } else {
    eTokenModeOpt[SC_MODE_GOST] = 1;
    Str16_1 = STRING_TOKEN (STR_ONE_OF_MODE_ETOKEN_PRO);
    Str16_2 = STRING_TOKEN (STR_ONE_OF_MODE_GOST);
  }

  Opt1 = HiiCreateOneOfOptionOpCode (OptionsOpCodeHandle,
    Str16_1, 0, EFI_IFR_NUMERIC_SIZE_1, 0);
  Opt2 = HiiCreateOneOfOptionOpCode (OptionsOpCodeHandle,
    Str16_2, 0, EFI_IFR_NUMERIC_SIZE_1, 1);

  if (NULL == Opt1 || NULL == Opt2) {
    DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
    goto _exit;
  }
  
  Status = HiiCreateOneOfOpCode (
      StartOpCodeHandle, QuestionId, 0, 0,
      Caption, STRING_TOKEN (STR_ONE_OF_PROMPT),
      EFI_IFR_FLAG_CALLBACK | EFI_IFR_FLAG_OPTIONS_ONLY,
      EFI_IFR_NUMERIC_SIZE_1,
      OptionsOpCodeHandle,
      NULL
      ) == NULL ? EFI_OUT_OF_RESOURCES : EFI_SUCCESS;
  DEBUG ((EFI_D_ERROR, "%a.%d Status=%X\n", __FUNCTION__, __LINE__, Status));
  HiiUpdateForm(HiiHandle, &FormSetGuid, 0x1000,
          StartOpCodeHandle, EndOpCodeHandle);
_exit:
  if (OptionsOpCodeHandle != NULL) {
    HiiFreeOpCodeHandle (OptionsOpCodeHandle);
  }
  return Status;
}
#endif


STATIC
EFI_STATUS
EFIAPI
FormCallback (
  IN  CONST EFI_HII_CONFIG_ACCESS_PROTOCOL   *This,
  IN  EFI_BROWSER_ACTION                     Action,
  IN  EFI_QUESTION_ID                        QuestionId,
  IN  UINT8                                  Type,
  IN  EFI_IFR_TYPE_VALUE                     *Value,
  OUT EFI_BROWSER_ACTION_REQUEST             *ActionRequest
  )
{
  EFI_STATUS Status;
  STATIC BOOLEAN bMenuCreated;
  
  DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
  DEBUG ((EFI_D_ERROR, "Action=%X\n", Action));
  DEBUG ((EFI_D_ERROR, "QuestionId=%X\n", QuestionId));
  DEBUG ((EFI_D_ERROR, "Type=%X\n", Type));

  switch (Action)  {
  case EFI_BROWSER_ACTION_FORM_OPEN:
    break;

  case EFI_BROWSER_ACTION_RETRIEVE:
#if 1
    if (!bMenuCreated) {
      bMenuCreated = TRUE;
      Status = AllocateHiiResources (gTokenPrivateData.HiiHandle);
      if (EFI_ERROR(Status)) {
        DEBUG ((EFI_D_ERROR, "%a.%d Error!\n", __FUNCTION__, __LINE__));
        break;
      }
      Status = eTokenModeOneOfString(
                gTokenPrivateData.HiiHandle,
                StartOpCodeHandle,
                EndOpCodeHandle,
                STRING_TOKEN (STR_ONE_OF_PROMPT),
                0x1234);
      DestroyHiiResources ();
      if (EFI_ERROR(Status)) {
        DEBUG ((EFI_D_ERROR, "%a.%d Error!\n", __FUNCTION__, __LINE__));
      } else {
        
      }
    }
#endif    
    break;
  
  case EFI_BROWSER_ACTION_FORM_CLOSE:
    bMenuCreated = FALSE;
    return EFI_SUCCESS;

  case EFI_BROWSER_ACTION_CHANGING:
    if (QuestionId == 0x1234 && Value != NULL) {
      EFI_GUID VarGuid = ETOKEN_SETUP_VAR_GUID;
      UINTN Size;
      ETOKEN_VARSTORE_DATA Data;
      Size = sizeof (ETOKEN_VARSTORE_DATA);
      Status = gRT->GetVariable (ETOKEN_VAR_NAME, &VarGuid, NULL, &Size, &Data);
      if (EFI_ERROR(Status)) {
        DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
        break;
      }
      DEBUG ((EFI_D_ERROR, "Value->u8=%X\n", Value->u8));
      if (Value->u8 == eTokenModeOpt[SC_MODE_PRO]) {
        //ETOKEN_MODE_PRO
        Data.eTokenMode = SC_MODE_PRO;
      } else {
        Data.eTokenMode = SC_MODE_GOST;
      }
      Status = gRT->SetVariable (
              ETOKEN_VAR_NAME, 
              &VarGuid,
              EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
              sizeof (Data),
              &Data);
      if (EFI_ERROR(Status)) {
        DEBUG ((EFI_D_ERROR, "%a.%d\n", __FUNCTION__, __LINE__));
        break;
      }
    }
    break;
  }
  
  return EFI_SUCCESS;
}



EFI_STATUS
EFIAPI
UsbCcidDriverBindingEntryPoint (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  EFI_STATUS              Status;

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gUsbCcidDriverBinding,
             ImageHandle,
             &gUsbCcidComponentName,
             &gUsbCcidComponentName2
             );

  ASSERT_EFI_ERROR (Status);

#if 1
{
  EFI_GUID FormSetGuid = ETOKEN_VFR_GUID;
  extern UINT8 eTokenVfrBin[];

  gTokenPrivateData.ConfigAccess.ExtractConfig = FakeExtractConfig;
  gTokenPrivateData.ConfigAccess.RouteConfig = FakeRouteConfig;
  gTokenPrivateData.ConfigAccess.Callback = FormCallback;

  Status = gBS->InstallProtocolInterface (
        &ImageHandle,
        &gEfiHiiConfigAccessProtocolGuid,
        EFI_NATIVE_INTERFACE,
        &gTokenPrivateData.ConfigAccess
        );
  
  gTokenPrivateData.HiiHandle = HiiAddPackages (
          &FormSetGuid,
          ImageHandle,
          eTokenVfrBin,
          eTokenDxeStrings, 
          NULL
          );
 
}
#endif
  return EFI_SUCCESS;
}

/**
  Starts the CCID device with this driver.

  @param  This                   The USB keyboard driver binding instance.
  @param  Controller             Handle of device to bind driver to.
  @param  RemainingDevicePath    Optional parameter use to pick a specific child
                                 device to start.

  @retval EFI_SUCCESS            The controller is controlled by the usb keyboard driver.
  @retval EFI_UNSUPPORTED        No interrupt endpoint can be found.
  @retval Other                  This controller cannot be started.

**/
EFI_STATUS
EFIAPI
eTokenCcidDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  )
{
  EFI_STATUS                    Status;
  EFI_USB_IO_PROTOCOL           *UsbIo;
  USB_CCID_DEV                  *UsbCcidDevice;
  UINT8                         EndpointNumber;
  EFI_USB_ENDPOINT_DESCRIPTOR   EndpointDescriptor;
  UINT8                         Index;
  UINT8                         EndpointAddr;
  UINT8                         PollingInterval;
  UINT8                         PacketSize;
  BOOLEAN                       Found;
  UINT8                         Mode;

  //
  // Open USB I/O Protocol
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiUsbIoProtocolGuid,
                  (VOID **) &UsbIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  UsbCcidDevice = AllocateZeroPool (sizeof (USB_CCID_DEV));
  ASSERT (UsbCcidDevice != NULL);

  //
  // Get the Device Path Protocol on Controller's handle
  //

  DEBUG(( EFI_D_ERROR, "%s: Device Remaining Type:%d SubType:%d Len:%d\n",
    L"eTokenCcidDriverBindingStart",
    RemainingDevicePath->Type,
    RemainingDevicePath->SubType,
    RemainingDevicePath->Length 
    ));


  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **) &UsbCcidDevice->DevicePath,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  UsbCcidDevice->hController = Controller;

  UsbCcidDevice->UsbIo = UsbIo;
  UsbCcidDevice->AgentHandle = This->DriverBindingHandle;
  UsbCcidDevice->bDriverStopped = FALSE;
  UsbCcidDevice->Locked = FALSE;
  EfiInitializeLock(&UsbCcidDevice->UsbCcidLock, TPL_CALLBACK);

  //
  // Get interface & endpoint descriptor
  //
  UsbIo->UsbGetInterfaceDescriptor (
           UsbIo,
           &UsbCcidDevice->InterfaceDescriptor
           );
#if 0
  Status = GetCcidDescriptor( UsbIo,
                              UsbCcidDevice->InterfaceDescriptor.InterfaceNumber,
                              &UsbCcidDevice->CcidDescriptor);

  EFI_ERROR_RET(Status, "Can't read CCID descriptor");

  if( (UsbCcidDevice->CcidDescriptor.bcdCCID != 0x100) &&
		  (UsbCcidDevice->CcidDescriptor.bcdCCID != 0x110) ) {
	  Status = EFI_UNSUPPORTED;
	  EFI_ERROR_RET(Status, "Not supported CCID version");
  }

  if( (UsbCcidDevice->CcidDescriptor.dwProtocols & SUPPORT_T0)) {
	  UsbCcidDevice->Flags |= FLAG_PROTOCOL_T0;
  } else
	  if( (UsbCcidDevice->CcidDescriptor.dwProtocols & SUPPORT_T1)) {
		  UsbCcidDevice->Flags |= FLAG_PROTOCOL_T1;
	  } else {
		  Status = EFI_UNSUPPORTED;
		  EFI_ERROR_RET(Status, "device does'n provide any supported protocols");
		  }



  /* "When a CCID doesn't declare the values 00000010h and 00000020h, the
   * frequency or the baud rate must be made via manufacturer proprietary
   * PC_to_RDR_Escape command." - ccid class specification v1.00
   *
   * "The value of the lower word (=0840) indicates that the host will only
   * send requests that are valid for the USB-ICC." - ISO/IEC 7816-12:2005
   * 7.2/Table 8
   */

  if( ((UsbCcidDevice->CcidDescriptor.dwFeatures & 0xffff) != 0x840) &&
		  (!(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTOMATIC_ICC_CLOCK_FRQ_CHG) ||
				  !(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTOMATIC_ICC_BAUD_RATE_CHG))
				  ) {
	  Status = EFI_UNSUPPORTED;
	  EFI_ERROR_RET(Status, "requred card initialization features missing");
  }

  // UsbCcidDevice->Protocols = UsbCcidDevice->CcidDescriptor.dwProtocols & ( SUPPORT_T0 | SUPPORT_T1 );
  // UsbCcidDevice->VoltageSupport = UsbCcidDevice->CcidDescriptor.bVoltageSupport;

  if( UsbCcidDevice->CcidDescriptor.bVoltageSupport & SUPPORT_VOLTAGE_18V ) {
	  UsbCcidDevice->Flags |= FLAG_VOLTAGE_18V;
  } else
	  if( UsbCcidDevice->CcidDescriptor.bVoltageSupport & SUPPORT_VOLTAGE_33V ) {
		  UsbCcidDevice->Flags |= FLAG_VOLTAGE_33V;
		  } else
			  if( UsbCcidDevice->CcidDescriptor.bVoltageSupport & SUPPORT_VOLTAGE_50V ) {
				  UsbCcidDevice->Flags |= FLAG_VOLTAGE_50V;
				  }

  if( UsbCcidDevice->CcidDescriptor.dwFeatures & TPDU_EXCHANGE_LEVEL ) {
	  UsbCcidDevice->ReaderType = TYPE_TPDU;
  } else if( (UsbCcidDevice->CcidDescriptor.dwFeatures & APDU_EXCHANGE_LEVEL) |
		  (UsbCcidDevice->CcidDescriptor.dwFeatures & APDU_EXT_EXCHANGE_LEVEL) ) {
	  UsbCcidDevice->ReaderType = TYPE_APDU;
  } else {
	  UsbCcidDevice->ReaderType = TYPE_CHAR;
  }

  if(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTO_CONFIG_ATR_PARSE) {
	  UsbCcidDevice->Flags |= FLAG_AUTO_ATRPARSE;
  }

  if(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTO_ACTIVATE) {
	  UsbCcidDevice->Flags |= FLAG_AUTO_ACTIVATE;
  }

  if(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTO_VOLTAGE) {
	  UsbCcidDevice->Flags |= FLAG_VOLTAGE_AUTO;
  }

  if(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTO_PARAM_NEG) {
	  UsbCcidDevice->Flags |= FLAG_NO_PTS | FLAG_NO_SETPARAM;
  }

  if(UsbCcidDevice->CcidDescriptor.dwFeatures & AUTO_PPS) {
	  UsbCcidDevice->Flags |= FLAG_NO_PTS;
  }

  if( UsbCcidDevice->CcidDescriptor.dwMaxCCIDMessageLength > CCID_MAX_MSG_LEN ) {
	  UsbCcidDevice->MaxMsgLength = CCID_MAX_MSG_LEN;
	  } else {
		  UsbCcidDevice->MaxMsgLength = UsbCcidDevice->CcidDescriptor.dwMaxCCIDMessageLength;
			}
  UsbCcidDevice->Slots = UsbCcidDevice->CcidDescriptor.bMaxSlotIndex + 1;
#endif

  UsbCcidDevice->CcidDescriptor.bMaxSlotIndex = 0;
  UsbCcidDevice->Slots = UsbCcidDevice->CcidDescriptor.bMaxSlotIndex + 1;
  EndpointNumber = UsbCcidDevice->InterfaceDescriptor.NumEndpoints = 2;


  //
  // Traverse endpoints
  //
  for (Index = 0; Index < EndpointNumber; Index++) {

    UsbIo->UsbGetEndpointDescriptor (
              UsbIo,
              Index,
              &EndpointDescriptor
              );

    if ((EndpointDescriptor.Attributes & (BIT0 | BIT1)) == USB_ENDPOINT_INTERRUPT) {
      //
      // We only care interrupt endpoint here
      //
      CopyMem(
              &UsbCcidDevice->IntEndpointDescriptor,
              &EndpointDescriptor,
              sizeof(EndpointDescriptor)
              );
      DEBUG((EFI_D_ERROR, "SC: Interrupt descriptor:%d", EndpointDescriptor.EndpointAddress));

      Found = TRUE;
      break;
    } else {
      if (EndpointDescriptor.EndpointAddress & BIT7) {
        CopyMem(
                &UsbCcidDevice->InEndpointDescriptor,
                &EndpointDescriptor,
                sizeof(EndpointDescriptor)
                );
        DEBUG((EFI_D_ERROR, "SC: Bulk In descriptor:%d\n", EndpointDescriptor.EndpointAddress));

      } else  {
        CopyMem(
              &UsbCcidDevice->OutEndpointDescriptor,
              &EndpointDescriptor,
              sizeof(EndpointDescriptor)
              );
        DEBUG((EFI_D_ERROR, "SC: Bulk Out descriptor:%d\n", EndpointDescriptor.EndpointAddress));
      }
    }
  }


  UsbCcidDevice->Signature = CCID_DEV_SIGNATURE;

//  InitializeListHead (&UsbCcidDevice->NotifyList);

  UsbCcidDevice->SmartCardReaderIo.Init = SmartCardReaderInit;
  UsbCcidDevice->SmartCardReaderIo.Finish = SmartCardReaderFinish;
  UsbCcidDevice->SmartCardReaderIo.Release = SmartCardReaderRelease;
  UsbCcidDevice->SmartCardReaderIo.Transmit = SmartCardReaderTransmit;
  UsbCcidDevice->SmartCardReaderIo.SendTpdu = SmartCardReaderSendTpdu;
  UsbCcidDevice->SmartCardReaderIo.Send = SmartCardReaderSend;
  UsbCcidDevice->SmartCardReaderIo.Receive = SmartCardReaderReceive;

  UsbCcidDevice->SmartCardReaderIo.Reset = SmartCardReaderReset;
  UsbCcidDevice->SmartCardReaderIo.GetStatus = SmartCardReaderGetStatus;

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Controller,
                  &gSmartCardReaderProtocolGuid,
                  &UsbCcidDevice->SmartCardReaderIo,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    goto ErrorExit;
  }

  //
  // Init USB CCID Device exhaustively.
  //

  Status = SmartCardReaderInit( &UsbCcidDevice->SmartCardReaderIo, NULL );

  if (EFI_ERROR (Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
          Controller,
          &gSmartCardReaderProtocolGuid,
          &UsbCcidDevice->SmartCardReaderIo,
          NULL
          );
    goto ErrorExit;
  }

  //
  // Submit Asynchronous Interrupt Transfer to manage this device.
  //

  if (EndpointNumber == 3) {
    EndpointAddr    = UsbCcidDevice->IntEndpointDescriptor.EndpointAddress;
    PollingInterval = UsbCcidDevice->IntEndpointDescriptor.Interval;
    PacketSize      = (UINT8) (UsbCcidDevice->IntEndpointDescriptor.MaxPacketSize);

    Status = UsbIo->UsbAsyncInterruptTransfer (
                    UsbIo,
                    EndpointAddr,
                    TRUE,
                    PollingInterval,
                    PacketSize,
                    CcidHandler,
                    UsbCcidDevice
                    );

    if (EFI_ERROR (Status)) {
      gBS->UninstallMultipleProtocolInterfaces (
                      Controller,
                      &gSmartCardReaderProtocolGuid,
                      &UsbCcidDevice->SmartCardReaderIo,
                      NULL
                      );
      goto ErrorExit;
    }
  }

  UsbCcidDevice->ControllerNameTable = NULL;

  AddUnicodeString2 (
    "eng",
    gUsbCcidComponentName.SupportedLanguages,
    &UsbCcidDevice->ControllerNameTable,
    L"Generic Usb Smartcard reader",
    TRUE
    );

  AddUnicodeString2 (
    "en",
    gUsbCcidComponentName2.SupportedLanguages,
    &UsbCcidDevice->ControllerNameTable,
    L"Generic Usb Smartcard reader",
    FALSE
    );

  for(Index = 0; Index < CCID_MAX_SLOTS; Index++) {
    UsbCcidDevice->State[Index].AtrBlobLength = 0;
    UsbCcidDevice->State[Index].Protocol = CCID_PROTOCOL_NONE;
  }

  if(EndpointNumber == 2) { // XXX if no interrupt event used then init fixed usb token
    UINTN Length = ATR_BLOB_MAX_SIZE;
    Status = UsbCcidDevice->SmartCardReaderIo.Reset(
                              &UsbCcidDevice->SmartCardReaderIo,
                              0, // Slot
                              UsbCcidDevice->State[0].AtrBlob,
                              &Length
                              );
    if (EFI_ERROR (Status)) {
      gBS->UninstallMultipleProtocolInterfaces (
              Controller,
              &gSmartCardReaderProtocolGuid,
              &UsbCcidDevice->SmartCardReaderIo,
              NULL
              );
      goto ErrorExit;
    }
    EFI_ERROR_RET(Status, "");
    UsbCcidDevice->State[0].AtrBlobLength = Length;

    DEBUG((EFI_D_ERROR, "ATR: "));

    for(Index = 0; Index < Length; Index++) {
      DEBUG((EFI_D_ERROR, "0x%x ",UsbCcidDevice->State[0].AtrBlob[Index]));
    }
    DEBUG((EFI_D_ERROR, "\n"));
#if 0
    Status = CcidSetProtocol(
                UsbCcidDevice ,
                0, // Slot
                CCID_PROTOCOL_T1
                );
    EFI_ERROR_RET(Status, "");
#endif
    UsbCcidDevice->SmartCard.FwVersion = eTokenGetFwVersion (
                                                UsbCcidDevice->State[0].AtrBlob, 
                                                Length);
  }
  UsbCcidDevice->SmartCard.VendorId = ETOKEN_VENDOR_ID;
  UsbCcidDevice->SmartCard.ProdId = ETOKEN_PROD_ID;

  Mode = SC_MODE_GOST;
  Status = GeteTokenScMode (&Mode);
  if (EFI_ERROR (Status) || Mode == SC_MODE_GOST) {
    UsbCcidDevice->SmartCard.WorkMode = ETOKEN_MODE_GOST_2001;
  } else {
    UsbCcidDevice->SmartCard.WorkMode = ETOKEN_MODE_PRO;
  }
  UsbCcidDevice->SmartCard.Atr = UsbCcidDevice->State[0].AtrBlob;
  UsbCcidDevice->SmartCard.AtrLen = UsbCcidDevice->State[0].AtrBlobLength;
  DEBUG((EFI_D_ERROR, "%a.%d FwVersion=%X\n", 
    __FUNCTION__, __LINE__, UsbCcidDevice->SmartCard.FwVersion));
  
  Status = SmartCardInstallProtocol( &Controller, UsbCcidDevice );
  if (EFI_ERROR(Status)) {
    gBS->UninstallMultipleProtocolInterfaces (
            Controller,
            &gSmartCardReaderProtocolGuid,
            &UsbCcidDevice->SmartCardReaderIo,
            NULL
            );
  } else {
    DEBUG((EFI_D_ERROR, "%s Success\n", L"eTokenCcidDriverBindingStart"));
    return EFI_SUCCESS;    
  }

//
// Error handler
//
OnError:
ErrorExit:
  DEBUG((EFI_D_ERROR, "%s Failed\n", L"eTokenCcidDriverBindingStart"));
  
  if (UsbCcidDevice != NULL) {
    FreePool (UsbCcidDevice);
    UsbCcidDevice = NULL;
  }
  gBS->CloseProtocol (
         Controller,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );
  return Status;

}

/**
  Stop the USB Ccid device handled by this driver.

  @param  This                   The USB keyboard driver binding protocol.
  @param  Controller             The controller to release.
  @param  NumberOfChildren       The number of handles in ChildHandleBuffer.
  @param  ChildHandleBuffer      The array of child handle.

  @retval EFI_SUCCESS            The device was stopped.
  @retval EFI_UNSUPPORTED        Simple Text In Protocol or Simple Text In Ex Protocol
                                 is not installed on Controller.
  @retval EFI_DEVICE_ERROR       The device could not be stopped due to a device error.
  @retval Others                 Fail to uninstall protocols attached on the device.

**/
EFI_STATUS
EFIAPI
eTokenCcidDriverBindingStop (
  IN  EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN  EFI_HANDLE                     Controller,
  IN  UINTN                          NumberOfChildren,
  IN  EFI_HANDLE                     *ChildHandleBuffer
  )
{
  EFI_STATUS                     Status;
  SMART_CARD_READER_PROTOCOL     *SmartCardReaderIo;
  USB_CCID_DEV                   *UsbCcidDevice;
  BOOLEAN                        bLockedByMe = FALSE;

    Status = gBS->OpenProtocol (
                  Controller,
                  &gSmartCardReaderProtocolGuid,
                  (VOID **) &SmartCardReaderIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );

  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }

  UsbCcidDevice = CR(
      SmartCardReaderIo,
      USB_CCID_DEV,
      SmartCardReaderIo,
      CCID_DEV_SIGNATURE
      );

  Status = EfiAcquireLockOrFail(&UsbCcidDevice->UsbCcidLock);
  if (EFI_ERROR(Status)) {
    bLockedByMe = FALSE;
    UsbCcidDevice->bDriverStopped = TRUE;
  } else {
    bLockedByMe = TRUE;
  }

  //
  // Delete the Asynchronous Interrupt Transfer from this device
  //

  if (UsbCcidDevice->InterfaceDescriptor.NumEndpoints == 3) {
    UsbCcidDevice->UsbIo->UsbAsyncInterruptTransfer (
                        UsbCcidDevice->UsbIo,
                        UsbCcidDevice->IntEndpointDescriptor.EndpointAddress,
                        FALSE,
                        UsbCcidDevice->IntEndpointDescriptor.Interval,
                        0,
                        NULL,
                        NULL
                        );
  }

  gBS->CloseProtocol (
         Controller,
         &gEfiUsbIoProtocolGuid,
         This->DriverBindingHandle,
         Controller
         );

  
  if (UsbCcidDevice->SmartCard.EjectNotify != NULL) {  
    UsbCcidDevice->SmartCard.EjectNotify(&UsbCcidDevice->SmartCard);
  }

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Controller,
                  &gSmartCardReaderProtocolGuid,
                  &UsbCcidDevice->SmartCardReaderIo,
                  NULL
                  );

  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Controller,
                  &gSmartCardProtocolGuid,
                  &UsbCcidDevice->SmartCard,
                  NULL
                  );

  //
  // Free all resources.
  //

  if (UsbCcidDevice->ControllerNameTable != NULL) {
    FreeUnicodeStringTable (UsbCcidDevice->ControllerNameTable);
  }

  if (bLockedByMe) {
    EfiReleaseLock(&UsbCcidDevice->UsbCcidLock);
    FreePool (UsbCcidDevice);
  }

  return Status;
}


