/* $Id$ */
/** @file
 * VBox USB R3 Driver Interface library
 */
/*
 * Copyright (C) 2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#define LOG_GROUP LOG_GROUP_DRV_USBPROXY
#include <windows.h>

#include <VBox/sup.h>
#include <VBox/types.h>
#include <VBox/err.h>
#include <VBox/param.h>
#include <iprt/path.h>
#include <iprt/assert.h>
#include <iprt/alloc.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <VBox/log.h>
#include <VBox/usblib.h>
#include <VBox/usblib-win.h>
#include <VBox/usb.h>
#include <VBox/VBoxDrvCfg-win.h>
#include <stdio.h>
#pragma warning (disable:4200) /* shuts up the empty array member warnings */
#include <setupapi.h>
#include <usbdi.h>
#include <hidsdi.h>

#define VBOX_USB_USE_DEVICE_NOTIFICATION

#ifdef VBOX_USB_USE_DEVICE_NOTIFICATION
# include <Dbt.h>
#endif

typedef struct _USB_INTERFACE_DESCRIPTOR2 {
    UCHAR  bLength;
    UCHAR  bDescriptorType;
    UCHAR  bInterfaceNumber;
    UCHAR  bAlternateSetting;
    UCHAR  bNumEndpoints;
    UCHAR  bInterfaceClass;
    UCHAR  bInterfaceSubClass;
    UCHAR  bInterfaceProtocol;
    UCHAR  iInterface;
    USHORT wNumClasses;
} USB_INTERFACE_DESCRIPTOR2, *PUSB_INTERFACE_DESCRIPTOR2;

typedef struct VBOXUSBGLOBALSTATE
{
    HANDLE hMonitor;
    HANDLE hNotifyEvent;
    HANDLE hInterruptEvent;
#ifdef VBOX_USB_USE_DEVICE_NOTIFICATION
    HANDLE hThread;
    HWND   hWnd;
    HANDLE hTimerQueue;
    HANDLE hTimer;
#endif
} VBOXUSBGLOBALSTATE, *PVBOXUSBGLOBALSTATE;

static VBOXUSBGLOBALSTATE g_VBoxUsbGlobal;

typedef struct VBOXUSB_STRING_DR_ENTRY
{
    struct VBOXUSB_STRING_DR_ENTRY *pNext;
    UCHAR iDr;
    USHORT idLang;
    USB_STRING_DESCRIPTOR StrDr;
} VBOXUSB_STRING_DR_ENTRY, *PVBOXUSB_STRING_DR_ENTRY;

/* this represents VBoxUsb device instance */
typedef struct VBOXUSB_DEV
{
    struct VBOXUSB_DEV *pNext;
    char    szName[512];
    char    szDriverRegName[512];
} VBOXUSB_DEV, *PVBOXUSB_DEV;

int usbLibVuDeviceValidate(PVBOXUSB_DEV pVuDev)
{
    HANDLE hOut = INVALID_HANDLE_VALUE;

    hOut = CreateFile(pVuDev->szName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, NULL);

    if (hOut == INVALID_HANDLE_VALUE)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CreateFile FAILED to open %s, winEr (%d)\n", pVuDev->szName, winEr));
        return VERR_GENERAL_FAILURE;
    }

    USBSUP_VERSION version = {0};
    DWORD cbReturned = 0;
    int rc = VERR_VERSION_MISMATCH;

    do
    {
        if (!DeviceIoControl(hOut, SUPUSB_IOCTL_GET_VERSION, NULL, 0,&version, sizeof(version),  &cbReturned, NULL))
        {
            AssertMsgFailed((__FUNCTION__": DeviceIoControl SUPUSB_IOCTL_GET_VERSION failed with LastError=%Rwa\n", GetLastError()));
            break;
        }

        if (version.u32Major != USBDRV_MAJOR_VERSION
                || version.u32Minor <  USBDRV_MINOR_VERSION)
        {
            AssertMsgFailed((__FUNCTION__": Invalid version %d:%d vs %d:%d\n", version.u32Major, version.u32Minor, USBDRV_MAJOR_VERSION, USBDRV_MINOR_VERSION));
            break;
        }

        if (!DeviceIoControl(hOut, SUPUSB_IOCTL_IS_OPERATIONAL, NULL, 0, NULL, NULL, &cbReturned, NULL))
        {
            AssertMsgFailed((__FUNCTION__": DeviceIoControl SUPUSB_IOCTL_IS_OPERATIONAL failed with LastError=%Rwa\n", GetLastError()));
            break;
        }

        rc = VINF_SUCCESS;
    } while (0);

    CloseHandle(hOut);
    return rc;
}

static int usbLibVuDevicePopulate(PVBOXUSB_DEV pVuDev, HDEVINFO hDevInfo, PSP_DEVICE_INTERFACE_DATA pIfData)
{
    DWORD cbIfDetailData;
    int rc = VINF_SUCCESS;

    SetupDiGetDeviceInterfaceDetail(hDevInfo, pIfData,
                NULL, /* OUT PSP_DEVICE_INTERFACE_DETAIL_DATA DeviceInterfaceDetailData */
                0, /* IN DWORD DeviceInterfaceDetailDataSize */
                &cbIfDetailData,
                NULL
                );
    Assert(GetLastError() == ERROR_INSUFFICIENT_BUFFER);

    PSP_DEVICE_INTERFACE_DETAIL_DATA pIfDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)RTMemAllocZ(cbIfDetailData);
    if (!pIfDetailData)
    {
        AssertMsgFailed((__FUNCTION__": RTMemAllocZ failed\n"));
        return VERR_OUT_OF_RESOURCES;
    }

    DWORD cbDbgRequired;
    SP_DEVINFO_DATA DevInfoData;
    DevInfoData.cbSize = sizeof (DevInfoData);
    /* the cbSize should contain the sizeof a fixed-size part according to the docs */
    pIfDetailData->cbSize = sizeof (*pIfDetailData);
    do
    {
        if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, pIfData,
                                pIfDetailData,
                                cbIfDetailData,
                                &cbDbgRequired,
                                &DevInfoData))
        {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": SetupDiGetDeviceInterfaceDetail, cbRequired (%d), was (%d), winEr (%d)\n", cbDbgRequired, cbIfDetailData, winEr));
            rc = VERR_GENERAL_FAILURE;
            break;
        }

        strncpy(pVuDev->szName, pIfDetailData->DevicePath, sizeof (pVuDev->szName));

        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &DevInfoData, SPDRP_DRIVER,
            NULL, /* OUT PDWORD PropertyRegDataType */
            (PBYTE)pVuDev->szDriverRegName,
            sizeof (pVuDev->szDriverRegName),
            &cbDbgRequired))
        {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": SetupDiGetDeviceRegistryPropertyA, cbRequired (%d), was (%d), winEr (%d)\n", cbDbgRequired, sizeof (pVuDev->szDriverRegName), winEr));
            rc = VERR_GENERAL_FAILURE;
            break;
        }

        rc = usbLibVuDeviceValidate(pVuDev);
        AssertRC(rc);
    } while (0);

    RTMemFree(pIfDetailData);
    return rc;
}

static void usbLibVuFreeDevices(PVBOXUSB_DEV pDevInfos)
{
    while (pDevInfos)
    {
        PVBOXUSB_DEV pNext = pDevInfos->pNext;
        RTMemFree(pDevInfos);
        pDevInfos = pNext;
    }
}

static int usbLibVuGetDevices(PVBOXUSB_DEV *ppVuDevs, uint32_t *pcVuDevs)
{
    *ppVuDevs = NULL;
    *pcVuDevs = 0;

    HDEVINFO hDevInfo =  SetupDiGetClassDevs(&GUID_CLASS_VBOXUSB,
            NULL, /* IN PCTSTR Enumerator */
            NULL, /* IN HWND hwndParent */
            (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE) /* IN DWORD Flags */
            );
    if (hDevInfo == INVALID_HANDLE_VALUE)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": SetupDiGetClassDevs, winEr (%d)\n", winEr));
        return VERR_GENERAL_FAILURE;
    }

    for (int i = 0; ; ++i)
    {
        SP_DEVICE_INTERFACE_DATA IfData;
        IfData.cbSize = sizeof (IfData);
        if (!SetupDiEnumDeviceInterfaces(hDevInfo,
                            NULL, /* IN PSP_DEVINFO_DATA DeviceInfoData */
                            &GUID_CLASS_VBOXUSB, /* IN LPGUID InterfaceClassGuid */
                            i,
                            &IfData))
        {
            DWORD winEr = GetLastError();
            if (winEr == ERROR_NO_MORE_ITEMS)
                break;

            AssertMsgFailed((__FUNCTION__": SetupDiEnumDeviceInterfaces, winEr (%d), resuming\n", winEr));
            continue;
        }

        /* we've now got the IfData */
        PVBOXUSB_DEV pVuDev = (PVBOXUSB_DEV)RTMemAllocZ(sizeof (*pVuDev));
        if (!pVuDev)
        {
            AssertMsgFailed((__FUNCTION__": RTMemAllocZ failed, resuming\n"));
            continue;
        }

        int rc = usbLibVuDevicePopulate(pVuDev, hDevInfo, &IfData);
        if (!RT_SUCCESS(rc))
        {
            AssertMsgFailed((__FUNCTION__": usbLibVuDevicePopulate failed, rc (%d), resuming\n", rc));
            continue;
        }

        pVuDev->pNext = *ppVuDevs;
        *ppVuDevs = pVuDev;
        ++*pcVuDevs;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);

    return VINF_SUCCESS;
}

static void usbLibDevFree(PUSBDEVICE pDevice)
{
    RTStrFree((char*)pDevice->pszAddress);
    RTStrFree((char*)pDevice->pszHubName);
    if (pDevice->pszManufacturer)
        RTStrFree((char*)pDevice->pszManufacturer);
    if (pDevice->pszProduct)
        RTStrFree((char*)pDevice->pszProduct);
    if (pDevice->pszSerialNumber)
        RTStrFree((char*)pDevice->pszSerialNumber);
    RTMemFree(pDevice);
}

static void usbLibDevFreeList(PUSBDEVICE pDevice)
{
    while (pDevice)
    {
        PUSBDEVICE pNext = pDevice->pNext;
        usbLibDevFree(pDevice);
        pDevice = pNext;
    }
}

static int usbLibDevPopulate(PUSBDEVICE pDev, PUSB_NODE_CONNECTION_INFORMATION_EX pConInfo, ULONG iPort, LPCSTR lpszDrvKeyName, LPCSTR lpszHubName, PVBOXUSB_STRING_DR_ENTRY pDrList)
{
    pDev->bcdUSB = pConInfo->DeviceDescriptor.bcdUSB;
    pDev->bDeviceClass = pConInfo->DeviceDescriptor.bDeviceClass;
    pDev->bDeviceSubClass = pConInfo->DeviceDescriptor.bDeviceSubClass;
    pDev->bDeviceProtocol = pConInfo->DeviceDescriptor.bDeviceProtocol;
    pDev->idVendor = pConInfo->DeviceDescriptor.idVendor;
    pDev->idProduct = pConInfo->DeviceDescriptor.idProduct;
    pDev->bcdDevice = pConInfo->DeviceDescriptor.bcdDevice;
    pDev->bBus = 0; /** @todo figure out bBus on windows... */
    pDev->bPort = iPort;
    /** @todo check which devices are used for primary input (keyboard & mouse) */
    if (!lpszDrvKeyName || *lpszDrvKeyName == 0)
        pDev->enmState = USBDEVICESTATE_UNUSED;
    else
        pDev->enmState = USBDEVICESTATE_USED_BY_HOST_CAPTURABLE;
    pDev->enmSpeed = USBDEVICESPEED_UNKNOWN;
    pDev->pszAddress = RTStrDup(lpszDrvKeyName);
    pDev->pszHubName = RTStrDup(lpszHubName);
    pDev->bNumConfigurations = 0;
    pDev->u64SerialHash = 0;

    for (; pDrList; pDrList = pDrList->pNext)
    {
        LPSTR *lppszString = NULL;
        if (pConInfo->DeviceDescriptor.iManufacturer && pDrList->iDr == pConInfo->DeviceDescriptor.iManufacturer)
        {
            lppszString = (LPSTR*)&pDev->pszManufacturer;
        }
        else if (pConInfo->DeviceDescriptor.iProduct && pDrList->iDr == pConInfo->DeviceDescriptor.iProduct)
        {
            lppszString = (LPSTR*)&pDev->pszProduct;
        }
        else if (pConInfo->DeviceDescriptor.iSerialNumber && pDrList->iDr == pConInfo->DeviceDescriptor.iSerialNumber)
        {
            lppszString = (LPSTR*)&pDev->pszSerialNumber;
        }

        if (lppszString)
        {
            char *pStringUTF8 = NULL;
            RTUtf16ToUtf8((PCRTUTF16)pDrList->StrDr.bString, &pStringUTF8);
            RTStrUtf8ToCurrentCP(lppszString, pStringUTF8);
            RTStrFree(pStringUTF8);
            if (pDrList->iDr == pConInfo->DeviceDescriptor.iSerialNumber)
            {
                pDev->u64SerialHash = USBLibHashSerial(pDev->pszSerialNumber);
            }
        }
    }

    return VINF_SUCCESS;
}

static void usbLibDevStrFree(LPSTR lpszName)
{
    RTStrFree(lpszName);
}

static int usbLibDevStrDriverKeyGet(HANDLE hHub, ULONG iPort, LPSTR* plpszName)
{
    USB_NODE_CONNECTION_DRIVERKEY_NAME Name;
    DWORD cbReturned = 0;
    Name.ConnectionIndex = iPort;
    *plpszName = NULL;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, &Name, sizeof (Name), &Name, sizeof (Name), &cbReturned, NULL))
    {
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": DeviceIoControl 1 fail winEr (%d)\n", winEr));
#endif
        return VERR_GENERAL_FAILURE;
    }

    if (Name.ActualLength < sizeof (Name))
    {
        AssertFailed();
        return VERR_OUT_OF_RESOURCES;
    }

    PUSB_NODE_CONNECTION_DRIVERKEY_NAME pName = (PUSB_NODE_CONNECTION_DRIVERKEY_NAME)RTMemAllocZ(Name.ActualLength);
    if (!pName)
    {
        AssertFailed();
        return VERR_OUT_OF_RESOURCES;
    }

    int rc = VINF_SUCCESS;
    pName->ConnectionIndex = iPort;
    if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, pName, Name.ActualLength, pName, Name.ActualLength, &cbReturned, NULL))
    {
        rc = RTUtf16ToUtf8Ex((PCRTUTF16)pName->DriverKeyName, pName->ActualLength, plpszName, 0, NULL);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
            rc = VINF_SUCCESS;
    }
    else
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": DeviceIoControl 2 fail winEr (%d)\n", winEr));
        rc = VERR_GENERAL_FAILURE;
    }
    RTMemFree(pName);
    return rc;
}

static int usbLibDevStrHubNameGet(HANDLE hHub, ULONG iPort, LPSTR* plpszName)
{
    USB_NODE_CONNECTION_NAME Name;
    DWORD cbReturned = 0;
    Name.ConnectionIndex = iPort;
    *plpszName = NULL;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_NAME, &Name, sizeof (Name), &Name, sizeof (Name), &cbReturned, NULL))
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    if (Name.ActualLength < sizeof (Name))
    {
        AssertFailed();
        return VERR_OUT_OF_RESOURCES;
    }

    PUSB_NODE_CONNECTION_NAME pName = (PUSB_NODE_CONNECTION_NAME)RTMemAllocZ(Name.ActualLength);
    if (!pName)
    {
        AssertFailed();
        return VERR_OUT_OF_RESOURCES;
    }

    int rc = VINF_SUCCESS;
    pName->ConnectionIndex = iPort;
    if (DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_NAME, pName, Name.ActualLength, pName, Name.ActualLength, &cbReturned, NULL))
    {
        rc = RTUtf16ToUtf8Ex((PCRTUTF16)pName->NodeName, pName->ActualLength, plpszName, 0, NULL);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
            rc = VINF_SUCCESS;
    }
    else
    {
        AssertFailed();
        rc = VERR_GENERAL_FAILURE;
    }
    RTMemFree(pName);
    return rc;
}

static int usbLibDevStrRootHubNameGet(HANDLE hCtl, LPSTR* plpszName)
{
    USB_ROOT_HUB_NAME HubName;
    DWORD cbReturned = 0;
    *plpszName = NULL;
    if (!DeviceIoControl(hCtl, IOCTL_USB_GET_ROOT_HUB_NAME, NULL, 0, &HubName, sizeof (HubName), &cbReturned, NULL))
    {
        return VERR_GENERAL_FAILURE;
    }
    PUSB_ROOT_HUB_NAME pHubName = (PUSB_ROOT_HUB_NAME)RTMemAllocZ(HubName.ActualLength);
    if (!pHubName)
        return VERR_OUT_OF_RESOURCES;

    int rc = VINF_SUCCESS;
    if (DeviceIoControl(hCtl, IOCTL_USB_GET_ROOT_HUB_NAME, NULL, 0, pHubName, HubName.ActualLength, &cbReturned, NULL))
    {
        rc = RTUtf16ToUtf8Ex((PCRTUTF16)pHubName->RootHubName, pHubName->ActualLength, plpszName, 0, NULL);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
            rc = VINF_SUCCESS;
    }
    else
    {
        rc = VERR_GENERAL_FAILURE;
    }
    RTMemFree(pHubName);
    return rc;
}

static int usbLibDevCfgDrGet(HANDLE hHub, ULONG iPort, ULONG iDr, PUSB_CONFIGURATION_DESCRIPTOR *ppDr)
{
    *ppDr = NULL;

    char Buf[sizeof (USB_DESCRIPTOR_REQUEST) + sizeof (USB_CONFIGURATION_DESCRIPTOR)];
    memset(&Buf, 0, sizeof (Buf));

    PUSB_DESCRIPTOR_REQUEST pCfgDrRq = (PUSB_DESCRIPTOR_REQUEST)Buf;
    PUSB_CONFIGURATION_DESCRIPTOR pCfgDr = (PUSB_CONFIGURATION_DESCRIPTOR)(Buf + sizeof (*pCfgDrRq));

    pCfgDrRq->ConnectionIndex = iPort;
    pCfgDrRq->SetupPacket.wValue = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | iDr;
    pCfgDrRq->SetupPacket.wLength = (USHORT)(sizeof (USB_CONFIGURATION_DESCRIPTOR));
    DWORD cbReturned = 0;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, pCfgDrRq, sizeof (Buf),
                                pCfgDrRq, sizeof (Buf),
                                &cbReturned, NULL))
    {
        DWORD winEr = GetLastError();
        LogRel((__FUNCTION__": DeviceIoControl 1 fail winEr (%d)\n", winEr));
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        AssertFailed();
#endif
        return VERR_GENERAL_FAILURE;
    }

    if (sizeof (Buf) != cbReturned)
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    if (pCfgDr->wTotalLength < sizeof (USB_CONFIGURATION_DESCRIPTOR))
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    DWORD cbRq = sizeof (USB_DESCRIPTOR_REQUEST) + pCfgDr->wTotalLength;
    PUSB_DESCRIPTOR_REQUEST pRq = (PUSB_DESCRIPTOR_REQUEST)RTMemAllocZ(cbRq);
    Assert(pRq);
    if (!pRq)
        return VERR_OUT_OF_RESOURCES;

    int rc = VERR_GENERAL_FAILURE;
    do
    {
        PUSB_CONFIGURATION_DESCRIPTOR pDr = (PUSB_CONFIGURATION_DESCRIPTOR)(pRq + 1);
        pRq->ConnectionIndex = iPort;
        pRq->SetupPacket.wValue = (USB_CONFIGURATION_DESCRIPTOR_TYPE << 8) | iDr;
        pRq->SetupPacket.wLength = (USHORT)(cbRq - sizeof (USB_DESCRIPTOR_REQUEST));
        if (!DeviceIoControl(hHub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, pRq, cbRq,
                                    pRq, cbRq,
                                    &cbReturned, NULL))
        {
            DWORD winEr = GetLastError();
            LogRel((__FUNCTION__": DeviceIoControl 2 fail winEr (%d)\n", winEr));
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
            AssertFailed();
#endif
            break;
        }

        if (cbRq != cbReturned)
        {
            AssertFailed();
            break;
        }

        if (pDr->wTotalLength != cbRq - sizeof (USB_DESCRIPTOR_REQUEST))
        {
            AssertFailed();
            break;
        }

        *ppDr = pDr;
        return VINF_SUCCESS;
    } while (0);

    RTMemFree(pRq);
    return rc;
}

static void usbLibDevCfgDrFree(PUSB_CONFIGURATION_DESCRIPTOR pDr)
{
    Assert(pDr);
    PUSB_DESCRIPTOR_REQUEST pRq = ((PUSB_DESCRIPTOR_REQUEST)pDr)-1;
    RTMemFree(pRq);
}

static int usbLibDevStrDrEntryGet(HANDLE hHub, ULONG iPort, ULONG iDr, USHORT idLang, PVBOXUSB_STRING_DR_ENTRY *ppList)
{
    char Buf[sizeof (USB_DESCRIPTOR_REQUEST) + MAXIMUM_USB_STRING_LENGTH];
    PUSB_DESCRIPTOR_REQUEST pRq = (PUSB_DESCRIPTOR_REQUEST)Buf;
    PUSB_STRING_DESCRIPTOR pDr = (PUSB_STRING_DESCRIPTOR)(Buf + sizeof (*pRq));
    memset (&Buf, 0, sizeof (Buf));
    pRq->ConnectionIndex = iPort;
    pRq->SetupPacket.wValue = (USB_STRING_DESCRIPTOR_TYPE << 8) | iDr;
    pRq->SetupPacket.wIndex = idLang;
    pRq->SetupPacket.wLength = sizeof (Buf) - sizeof (*pRq);
    DWORD cbReturned = 0;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, pRq, sizeof (Buf),
                                    pRq, sizeof (Buf),
                                    &cbReturned, NULL))
    {
        DWORD winEr = GetLastError();
        LogRel((__FUNCTION__": DeviceIoControl 1 fail winEr (%d)\n", winEr));
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
            AssertFailed();
#endif
        return VERR_GENERAL_FAILURE;
    }

    if (cbReturned < sizeof (*pDr) + 2)
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    if (!!(pDr->bLength % 2))
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    if (pDr->bLength != cbReturned - sizeof (*pRq))
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    if (pDr->bDescriptorType != USB_STRING_DESCRIPTOR_TYPE)
    {
        AssertFailed();
        return VERR_GENERAL_FAILURE;
    }

    PVBOXUSB_STRING_DR_ENTRY pEntry = (PVBOXUSB_STRING_DR_ENTRY)RTMemAllocZ(sizeof (*pEntry) + pDr->bLength + 2);
    Assert(pEntry);
    if (!pEntry)
    {
        return VERR_OUT_OF_RESOURCES;
    }

    pEntry->pNext = *ppList;
    pEntry->iDr = iDr;
    pEntry->idLang = idLang;
    memcpy(&pEntry->StrDr, pDr, pDr->bLength);
    *ppList = pEntry;
    return VINF_SUCCESS;
}

static void usbLibDevStrDrEntryFree(PVBOXUSB_STRING_DR_ENTRY pDr)
{
    RTMemFree(pDr);
}

static void usbLibDevStrDrEntryFreeList(PVBOXUSB_STRING_DR_ENTRY pDr)
{
    while (pDr)
    {
        PVBOXUSB_STRING_DR_ENTRY pNext = pDr->pNext;
        usbLibDevStrDrEntryFree(pDr);
        pDr = pNext;
    }
}

static int usbLibDevStrDrEntryGetForLangs(HANDLE hHub, ULONG iPort, ULONG iDr, ULONG cIdLang, const USHORT *pIdLang, PVBOXUSB_STRING_DR_ENTRY *ppList)
{
    for (ULONG i = 0; i < cIdLang; ++i)
    {
        usbLibDevStrDrEntryGet(hHub, iPort, iDr, pIdLang[i], ppList);
    }
    return VINF_SUCCESS;
}

static int usbLibDevStrDrEntryGetAll(HANDLE hHub, ULONG iPort, PUSB_DEVICE_DESCRIPTOR pDevDr, PUSB_CONFIGURATION_DESCRIPTOR pCfgDr, PVBOXUSB_STRING_DR_ENTRY *ppList)
{
    int rc = usbLibDevStrDrEntryGet(hHub, iPort, 0, 0, ppList);
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
    AssertRC(rc);
#endif
    if (RT_FAILURE(rc))
        return rc;

    PUSB_STRING_DESCRIPTOR pLandStrDr = &(*ppList)->StrDr;
    USHORT *pIdLang = pLandStrDr->bString;
    ULONG cIdLang = (pLandStrDr->bLength - RT_OFFSETOF(USB_STRING_DESCRIPTOR, bString)) / sizeof (*pIdLang);

    if (pDevDr->iManufacturer)
    {
        rc = usbLibDevStrDrEntryGetForLangs(hHub, iPort, pDevDr->iManufacturer, cIdLang, pIdLang, ppList);
        AssertRC(rc);
    }

    if (pDevDr->iProduct)
    {
        rc = usbLibDevStrDrEntryGetForLangs(hHub, iPort, pDevDr->iProduct, cIdLang, pIdLang, ppList);
        AssertRC(rc);
    }

    if (pDevDr->iSerialNumber)
    {
        rc = usbLibDevStrDrEntryGetForLangs(hHub, iPort, pDevDr->iSerialNumber, cIdLang, pIdLang, ppList);
        AssertRC(rc);
    }

    PUCHAR pCur = (PUCHAR)pCfgDr;
    PUCHAR pEnd = pCur + pCfgDr->wTotalLength;
    while (pCur + sizeof (USB_COMMON_DESCRIPTOR) <= pEnd)
    {
        PUSB_COMMON_DESCRIPTOR pCmnDr = (PUSB_COMMON_DESCRIPTOR)pCur;
        if (pCur + pCmnDr->bLength > pEnd)
        {
            AssertFailed();
            break;
        }

        switch (pCmnDr->bDescriptorType)
        {
            case USB_CONFIGURATION_DESCRIPTOR_TYPE:
            {
                if (pCmnDr->bLength != sizeof (USB_CONFIGURATION_DESCRIPTOR))
                {
                    AssertFailed();
                    break;
                }
                PUSB_CONFIGURATION_DESCRIPTOR pCurCfgDr = (PUSB_CONFIGURATION_DESCRIPTOR)pCmnDr;
                if (!pCurCfgDr->iConfiguration)
                    break;
                rc = usbLibDevStrDrEntryGetForLangs(hHub, iPort, pCurCfgDr->iConfiguration, cIdLang, pIdLang, ppList);
                AssertRC(rc);
                break;
            }
            case USB_INTERFACE_DESCRIPTOR_TYPE:
            {
                if (pCmnDr->bLength != sizeof (USB_INTERFACE_DESCRIPTOR) && pCmnDr->bLength != sizeof (USB_INTERFACE_DESCRIPTOR2))
                {
                    AssertFailed();
                    break;
                }
                PUSB_INTERFACE_DESCRIPTOR pCurIfDr = (PUSB_INTERFACE_DESCRIPTOR)pCmnDr;
                if (!pCurIfDr->iInterface)
                    break;
                rc = usbLibDevStrDrEntryGetForLangs(hHub, iPort, pCurIfDr->iInterface, cIdLang, pIdLang, ppList);
                AssertRC(rc);
                break;
            }
            default:
                break;
        }

        pCur = pCur + pCmnDr->bLength;
    }

    return VINF_SUCCESS;
}

static int usbLibDevGetHubDevices(LPCSTR lpszName, PUSBDEVICE *ppDevs, uint32_t *pcDevs);

static int usbLibDevGetHubPortDevices(HANDLE hHub, LPCSTR lpcszHubName, ULONG iPort, PUSBDEVICE *ppDevs, uint32_t *pcDevs)
{
    int rc = VINF_SUCCESS;
    char Buf[sizeof (USB_NODE_CONNECTION_INFORMATION_EX) + (sizeof (USB_PIPE_INFO) * 20)];
    PUSB_NODE_CONNECTION_INFORMATION_EX pConInfo = (PUSB_NODE_CONNECTION_INFORMATION_EX)Buf;
    PUSB_PIPE_INFO paPipeInfo = (PUSB_PIPE_INFO)(Buf + sizeof (PUSB_NODE_CONNECTION_INFORMATION_EX));
    DWORD cbReturned = 0;
    memset(&Buf, 0, sizeof (Buf));
    pConInfo->ConnectionIndex = iPort;
    if (!DeviceIoControl(hHub, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX,
                                  pConInfo, sizeof (Buf),
                                  pConInfo, sizeof (Buf),
                                  &cbReturned, NULL))
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": DeviceIoControl failed winEr (%d)\n", winEr));
        return VERR_GENERAL_FAILURE;
    }

    if (pConInfo->ConnectionStatus != DeviceConnected)
    {
        /* just ignore & return success */
        return VWRN_INVALID_HANDLE;
    }

    if (pConInfo->DeviceIsHub)
    {
        LPSTR lpszHubName = NULL;
        rc = usbLibDevStrHubNameGet(hHub, iPort, &lpszHubName);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            rc = usbLibDevGetHubDevices(lpszHubName, ppDevs, pcDevs);
            usbLibDevStrFree(lpszHubName);
            AssertRC(rc);
            return rc;
        }
        /* ignore this err */
        return VINF_SUCCESS;
    }

    LPSTR lpszName = NULL;
    rc = usbLibDevStrDriverKeyGet(hHub, iPort, &lpszName);
    if (RT_FAILURE(rc))
    {
        return rc;
    }

    Assert(lpszName);

    PUSB_CONFIGURATION_DESCRIPTOR pCfgDr = NULL;
    PVBOXUSB_STRING_DR_ENTRY pList = NULL;
    rc = usbLibDevCfgDrGet(hHub, iPort, 0, &pCfgDr);
    if (pCfgDr)
    {
        rc = usbLibDevStrDrEntryGetAll(hHub, iPort, &pConInfo->DeviceDescriptor, pCfgDr, &pList);
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        AssertRC(rc);
#endif
    }

    PUSBDEVICE pDev = (PUSBDEVICE)RTMemAllocZ(sizeof (*pDev));
    rc = usbLibDevPopulate(pDev, pConInfo, iPort, lpszName, lpcszHubName, pList);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        pDev->pNext = *ppDevs;
        *ppDevs = pDev;
        ++*pcDevs;
    }

    if (pCfgDr)
        usbLibDevCfgDrFree(pCfgDr);
    if (lpszName)
        usbLibDevStrFree(lpszName);
    if (pList)
        usbLibDevStrDrEntryFreeList(pList);

    return VINF_SUCCESS;
}

static int usbLibDevGetHubDevices(LPCSTR lpszName, PUSBDEVICE *ppDevs, uint32_t *pcDevs)
{
    LPSTR lpszDevName = (LPSTR)RTMemAllocZ(strlen(lpszName) + sizeof("\\\\.\\"));
    Assert(lpszDevName);
    if (!lpszDevName)
    {
        AssertFailed();
        return VERR_OUT_OF_RESOURCES;
    }

    int rc = VINF_SUCCESS;
    strcpy(lpszDevName, "\\\\.\\");
    strcpy(lpszDevName + sizeof("\\\\.\\") - sizeof (lpszDevName[0]), lpszName);
    do
    {
        DWORD cbReturned = 0;
        HANDLE hDev = CreateFile(lpszDevName, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDev == INVALID_HANDLE_VALUE)
        {
            AssertFailed();
            break;
        }

        USB_NODE_INFORMATION NodeInfo;
        memset(&NodeInfo, 0, sizeof (NodeInfo));
        if (!DeviceIoControl(hDev, IOCTL_USB_GET_NODE_INFORMATION,
                            &NodeInfo, sizeof (NodeInfo),
                            &NodeInfo, sizeof (NodeInfo),
                            &cbReturned, NULL))
        {
            AssertFailed();
            break;
        }

        for (ULONG i = 1; i <= NodeInfo.u.HubInformation.HubDescriptor.bNumberOfPorts; ++i)
        {
            usbLibDevGetHubPortDevices(hDev, lpszName, i, ppDevs, pcDevs);
        }
    } while (0);
    RTMemFree(lpszDevName);

    return rc;
}

static int usbLibDevGetDevices(PUSBDEVICE *ppDevs, uint32_t *pcDevs)
{
    char CtlName[16];
    int rc = VINF_SUCCESS;

    for (int i = 0; i < 10; ++i)
    {
        sprintf(CtlName, "\\\\.\\HCD%d", i);
        HANDLE hCtl = CreateFile(CtlName, GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hCtl != INVALID_HANDLE_VALUE)
        {
            char* lpszName;
            rc = usbLibDevStrRootHubNameGet(hCtl, &lpszName);
            AssertRC(rc);
            if (RT_SUCCESS(rc))
            {
                rc = usbLibDevGetHubDevices(lpszName, ppDevs, pcDevs);
                AssertRC(rc);
                usbLibDevStrFree(lpszName);
            }
            CloseHandle(hCtl);
            if (RT_FAILURE(rc))
                break;
        }
    }
    return VINF_SUCCESS;
}

static PUSBSUP_GET_DEVICES usbLibMonGetDevRqAlloc(uint32_t cDevs, PDWORD pcbRq)
{
    DWORD cbRq = RT_OFFSETOF(USBSUP_GET_DEVICES, aDevices[cDevs]);
    PUSBSUP_GET_DEVICES pRq = (PUSBSUP_GET_DEVICES)RTMemAllocZ(cbRq);
    Assert(pRq);
    if (!pRq)
        return NULL;
    pRq->cDevices = cDevs;
    *pcbRq = cbRq;
    return pRq;
}

static int usbLibMonDevicesCmp(PUSBDEVICE pDev, PVBOXUSB_DEV pDevInfo)
{
    int iDiff;
    iDiff = strcmp(pDev->pszAddress, pDevInfo->szDriverRegName);
    return iDiff;
}

static int usbLibMonDevicesUpdate(PVBOXUSBGLOBALSTATE pGlobal, PUSBDEVICE pDevs, uint32_t cDevs, PVBOXUSB_DEV pDevInfos, uint32_t cDevInfos)
{
    PUSBDEVICE pDevsHead = pDevs;
    for (; pDevInfos; pDevInfos = pDevInfos->pNext)
    {
        for (pDevs = pDevsHead; pDevs; pDevs = pDevs->pNext)
        {
            if (usbLibMonDevicesCmp(pDevs, pDevInfos))
                continue;

            if (!pDevInfos->szDriverRegName[0])
            {
                AssertFailed();
                break;
            }

            USBSUP_GETDEV Dev = {0};
            HANDLE hDev = CreateFile(pDevInfos->szName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, NULL,
                                                          OPEN_EXISTING,  FILE_ATTRIBUTE_SYSTEM, NULL);
            if (hDev == INVALID_HANDLE_VALUE)
            {
                AssertFailed();
                break;
            }

            DWORD cbReturned = 0;
            if (!DeviceIoControl(hDev, SUPUSB_IOCTL_GET_DEVICE, &Dev, sizeof (Dev), &Dev, sizeof (Dev), &cbReturned, NULL))
            {
                 DWORD winEr = GetLastError();
                 /* ERROR_DEVICE_NOT_CONNECTED -> device was removed just now */
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
                 AssertMsg(winEr == ERROR_DEVICE_NOT_CONNECTED, (__FUNCTION__": DeviceIoControl failed winEr (%d)\n", winEr));
#endif
                 Log(("SUPUSB_IOCTL_GET_DEVICE: DeviceIoControl no longer connected\n"));
                 CloseHandle(hDev);
                 break;
            }

            /* we must not close the handle until we request for the device state from the monitor to ensure
             * the device handle returned by the device driver does not disappear */
            Assert(Dev.hDevice);
            USBSUP_GETDEV_MON MonInfo;
            HVBOXUSBDEVUSR hDevice = Dev.hDevice;
            if (!DeviceIoControl(pGlobal->hMonitor, SUPUSBFLT_IOCTL_GET_DEVICE, &hDevice, sizeof (hDevice), &MonInfo, sizeof (MonInfo), &cbReturned, NULL))
            {
                 DWORD winEr = GetLastError();
                 /* ERROR_DEVICE_NOT_CONNECTED -> device was removed just now */
                 AssertMsgFailed((__FUNCTION__": Monitor DeviceIoControl failed winEr (%d)\n", winEr));
                 Log(("SUPUSBFLT_IOCTL_GET_DEVICE: DeviceIoControl no longer connected\n"));
                 CloseHandle(hDev);
                 break;
            }

            CloseHandle(hDev);

            /* success!! update device info */
            /* ensure the state returned is valid */
            Assert(    MonInfo.enmState == USBDEVICESTATE_USED_BY_HOST
                    || MonInfo.enmState == USBDEVICESTATE_USED_BY_HOST_CAPTURABLE
                    || MonInfo.enmState == USBDEVICESTATE_UNUSED
                    || MonInfo.enmState == USBDEVICESTATE_HELD_BY_PROXY
                    || MonInfo.enmState == USBDEVICESTATE_USED_BY_GUEST);
            pDevs->enmState = MonInfo.enmState;
            /* The following is not 100% accurate but we only care about high-speed vs. non-high-speed */
            pDevs->enmSpeed = Dev.fHiSpeed ? USBDEVICESPEED_HIGH : USBDEVICESPEED_FULL;
            if (pDevs->enmState != USBDEVICESTATE_USED_BY_HOST)
            {
                /* only set the interface name if device can be grabbed */
                RTStrFree(pDevs->pszAltAddress);
                pDevs->pszAltAddress = (char*)pDevs->pszAddress;
                pDevs->pszAddress = RTStrDup(pDevInfos->szName);
            }
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
            else
            {
                /* dbg breakpoint */
                Assert(0);
            }
#endif

            /* we've found the device, break in any way */
            break;
        }
    }

    return VINF_SUCCESS;
}

static int usbLibGetDevices(PVBOXUSBGLOBALSTATE pGlobal, PUSBDEVICE *ppDevs, uint32_t *pcDevs)
{
    *ppDevs = NULL;
    *pcDevs = 0;

    int rc = usbLibDevGetDevices(ppDevs, pcDevs);
    AssertRC(rc);
    if (RT_SUCCESS(rc))
    {
        PVBOXUSB_DEV pDevInfos = NULL;
        uint32_t cDevInfos = 0;
        rc = usbLibVuGetDevices(&pDevInfos, &cDevInfos);
        AssertRC(rc);
        if (RT_SUCCESS(rc))
        {
            rc = usbLibMonDevicesUpdate(pGlobal, *ppDevs, *pcDevs, pDevInfos, cDevInfos);
            AssertRC(rc);
            usbLibVuFreeDevices(pDevInfos);
        }

        return VINF_SUCCESS;
    }
    return rc;
}

AssertCompile(INFINITE == RT_INDEFINITE_WAIT);
static int usbLibStateWaitChange(PVBOXUSBGLOBALSTATE pGlobal, RTMSINTERVAL cMillies)
{
    HANDLE ahEvents[] = {pGlobal->hNotifyEvent, pGlobal->hInterruptEvent};
    DWORD dwResult = WaitForMultipleObjects(RT_ELEMENTS(ahEvents), ahEvents,
                        FALSE, /* BOOL bWaitAll */
                        cMillies);

    switch (dwResult)
    {
        case WAIT_OBJECT_0:
            return VINF_SUCCESS;
        case WAIT_OBJECT_0 + 1:
            return VERR_INTERRUPTED;
        case WAIT_TIMEOUT:
            return VERR_TIMEOUT;
        default:
        {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": WaitForMultipleObjects failed, winEr (%d)\n", winEr));
            return VERR_GENERAL_FAILURE;
        }
    }
}

AssertCompile(RT_INDEFINITE_WAIT == INFINITE);
AssertCompile(sizeof (RTMSINTERVAL) == sizeof (DWORD));
USBLIB_DECL(int) USBLibWaitChange(RTMSINTERVAL msWaitTimeout)
{
    return usbLibStateWaitChange(&g_VBoxUsbGlobal, msWaitTimeout);
}

static int usbLibInterruptWaitChange(PVBOXUSBGLOBALSTATE pGlobal)
{
    BOOL bRc = SetEvent(pGlobal->hInterruptEvent);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": SetEvent failed, winEr (%d)\n", winEr));
        return VERR_GENERAL_FAILURE;
    }
    return VINF_SUCCESS;
}

USBLIB_DECL(int) USBLibInterruptWaitChange()
{
    return usbLibInterruptWaitChange(&g_VBoxUsbGlobal);
}

/*
USBLIB_DECL(bool) USBLibHasPendingDeviceChanges(void)
{
    int rc = USBLibWaitChange(0);
    return rc == VINF_SUCCESS;
}
*/

USBLIB_DECL(int) USBLibGetDevices(PUSBDEVICE *ppDevices, uint32_t *pcbNumDevices)
{
    Assert(g_VBoxUsbGlobal.hMonitor != INVALID_HANDLE_VALUE);
    return usbLibGetDevices(&g_VBoxUsbGlobal, ppDevices, pcbNumDevices);
}

USBLIB_DECL(void *) USBLibAddFilter(PCUSBFILTER pFilter)
{
    USBSUP_FLTADDOUT FltAddRc;
    DWORD cbReturned = 0;

    if (g_VBoxUsbGlobal.hMonitor == INVALID_HANDLE_VALUE)
    {
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        AssertFailed();
#endif
        return NULL;
    }

    Log(("usblibInsertFilter: Manufacturer=%s Product=%s Serial=%s\n",
         USBFilterGetString(pFilter, USBFILTERIDX_MANUFACTURER_STR)  ? USBFilterGetString(pFilter, USBFILTERIDX_MANUFACTURER_STR)  : "<null>",
         USBFilterGetString(pFilter, USBFILTERIDX_PRODUCT_STR)       ? USBFilterGetString(pFilter, USBFILTERIDX_PRODUCT_STR)       : "<null>",
         USBFilterGetString(pFilter, USBFILTERIDX_SERIAL_NUMBER_STR) ? USBFilterGetString(pFilter, USBFILTERIDX_SERIAL_NUMBER_STR) : "<null>"));

    if (!DeviceIoControl(g_VBoxUsbGlobal.hMonitor, SUPUSBFLT_IOCTL_ADD_FILTER,
                (LPVOID)pFilter, sizeof(*pFilter),
                &FltAddRc, sizeof(FltAddRc),
                &cbReturned, NULL))
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed(("DeviceIoControl failed with winEr (%d(\n", winEr));
        return NULL;
    }

    if (RT_FAILURE(FltAddRc.rc))
    {
        AssertMsgFailed(("Adding filter failed with %d\n", FltAddRc.rc));
        return NULL;
    }
    return (void *)FltAddRc.uId;
}


USBLIB_DECL(void) USBLibRemoveFilter(void *pvId)
{
    uintptr_t uId;
    DWORD cbReturned = 0;

    if (g_VBoxUsbGlobal.hMonitor == INVALID_HANDLE_VALUE)
    {
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        AssertFailed();
#endif
        return;
    }

    Log(("usblibRemoveFilter %p\n", pvId));

    uId = (uintptr_t)pvId;
    if (!DeviceIoControl(g_VBoxUsbGlobal.hMonitor, SUPUSBFLT_IOCTL_REMOVE_FILTER, &uId, sizeof(uId),  NULL, 0,&cbReturned, NULL))
        AssertMsgFailed(("DeviceIoControl failed with LastError=%Rwa\n", GetLastError()));
}

USBLIB_DECL(int) USBLibRunFilters()
{
    DWORD cbReturned = 0;

    Assert(g_VBoxUsbGlobal.hMonitor != INVALID_HANDLE_VALUE);

    if (!DeviceIoControl(g_VBoxUsbGlobal.hMonitor, SUPUSBFLT_IOCTL_RUN_FILTERS,
                NULL, 0,
                NULL, 0,
                &cbReturned, NULL))
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed(("DeviceIoControl failed with winEr (%d(\n", winEr));
        return RTErrConvertFromWin32(winEr);
    }

    return VINF_SUCCESS;
}


#ifdef VBOX_USB_USE_DEVICE_NOTIFICATION

static VOID CALLBACK usbLibTimerCallback(
        __in  PVOID lpParameter,
        __in  BOOLEAN TimerOrWaitFired
      )
{
    SetEvent(g_VBoxUsbGlobal.hNotifyEvent);
}

static void usbLibOnDeviceChange()
{
    /* we're getting series of events like that especially on device re-attach
     * (i.e. first for device detach and then for device attach)
     * unfortunately the event does not tell us what actually happened.
     * To avoid extra notifications, we delay the SetEvent via a timer
     * and update the timer if additional notification comes before the timer fires
     * */
    if (g_VBoxUsbGlobal.hTimer)
    {
        if (!DeleteTimerQueueTimer(g_VBoxUsbGlobal.hTimerQueue, g_VBoxUsbGlobal.hTimer, NULL))
        {
            DWORD winEr = GetLastError();
            AssertMsg(winEr == ERROR_IO_PENDING, (__FUNCTION__": DeleteTimerQueueTimer failed, winEr (%d)\n", winEr));
        }
    }

    if (!CreateTimerQueueTimer(&g_VBoxUsbGlobal.hTimer, g_VBoxUsbGlobal.hTimerQueue,
                                        usbLibTimerCallback,
                                        NULL,
                                        500, /* ms*/
                                        0,
                                        WT_EXECUTEONLYONCE))
    {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": CreateTimerQueueTimer failed, winEr (%d)\n", winEr));

            /* call it directly */
            usbLibTimerCallback(NULL, FALSE);
    }
}

static LRESULT CALLBACK usbLibWndProc(HWND hwnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (uMsg)
    {
        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVNODES_CHANGED)
            {
                /* we notify change any device arivals/removals on the system
                 * and let the client decide whether the usb change actually happened
                 * so far this is more clean than reporting events from the Monitor
                 * because monitor sees only PDO arrivals/removals,
                 * and by the time PDO is created, device can not
                 * be yet started and fully functional,
                 * so usblib won't be able to pick it up
                 * */

                usbLibOnDeviceChange();
            }
            break;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProc (hwnd, uMsg, wParam, lParam);
}

static LPCSTR g_VBoxUsbWndClassName = "VBoxUsbLibClass";

static DWORD WINAPI usbLibMsgThreadProc(__in  LPVOID lpParameter)
{
     HWND                 hwnd = 0;
     HINSTANCE hInstance = (HINSTANCE)GetModuleHandle (NULL);
     bool bExit = false;

     /* Register the Window Class. */
     WNDCLASS wc;
     wc.style         = 0;
     wc.lpfnWndProc   = usbLibWndProc;
     wc.cbClsExtra    = 0;
     wc.cbWndExtra    = sizeof(void *);
     wc.hInstance     = hInstance;
     wc.hIcon         = NULL;
     wc.hCursor       = NULL;
     wc.hbrBackground = (HBRUSH)(COLOR_BACKGROUND + 1);
     wc.lpszMenuName  = NULL;
     wc.lpszClassName = g_VBoxUsbWndClassName;

     ATOM atomWindowClass = RegisterClass(&wc);

     if (atomWindowClass != 0)
     {
         /* Create the window. */
         g_VBoxUsbGlobal.hWnd = CreateWindowEx (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                 g_VBoxUsbWndClassName, g_VBoxUsbWndClassName,
                                                   WS_POPUPWINDOW,
                                                  -200, -200, 100, 100, NULL, NULL, hInstance, NULL);
         SetEvent(g_VBoxUsbGlobal.hNotifyEvent);

         if (g_VBoxUsbGlobal.hWnd)
         {
             SetWindowPos(hwnd, HWND_TOPMOST, -200, -200, 0, 0,
                          SWP_NOACTIVATE | SWP_HIDEWINDOW | SWP_NOCOPYBITS | SWP_NOREDRAW | SWP_NOSIZE);

             MSG msg;
             while (GetMessage(&msg, NULL, 0, 0))
             {
                 TranslateMessage(&msg);
                 DispatchMessage(&msg);
             }

             DestroyWindow (hwnd);

             bExit = true;
         }

         UnregisterClass (g_VBoxUsbWndClassName, hInstance);
     }

     if(bExit)
     {
         /* no need any accuracy here, in anyway the DHCP server usually gets terminated with TerminateProcess */
         exit(0);
     }

     return 0;
}
#endif

/**
 * Initialize the USB library
 *
 * @returns VBox status code.
 */
USBLIB_DECL(int) USBLibInit(void)
{
    int rc = VERR_GENERAL_FAILURE;

    Log(("usbproxy: usbLibInit\n"));

    memset(&g_VBoxUsbGlobal, 0, sizeof (g_VBoxUsbGlobal));

    g_VBoxUsbGlobal.hMonitor = INVALID_HANDLE_VALUE;

    g_VBoxUsbGlobal.hNotifyEvent = CreateEvent(NULL, /* LPSECURITY_ATTRIBUTES lpEventAttributes */
                                        FALSE, /* BOOL bManualReset */
#ifndef VBOX_USB_USE_DEVICE_NOTIFICATION
                                        TRUE,  /* BOOL bInitialState */
#else
                                        FALSE, /* set to false since it will be initially used for notification thread startup sync */
#endif
                                        NULL /* LPCTSTR lpName */);
    if (g_VBoxUsbGlobal.hNotifyEvent)
    {
        g_VBoxUsbGlobal.hInterruptEvent = CreateEvent(NULL, /* LPSECURITY_ATTRIBUTES lpEventAttributes */
                                                FALSE, /* BOOL bManualReset */
                                                FALSE,  /* BOOL bInitialState */
                                                NULL /* LPCTSTR lpName */);
        if (g_VBoxUsbGlobal.hInterruptEvent)
        {
            g_VBoxUsbGlobal.hMonitor = CreateFile(USBMON_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, NULL);

            if (g_VBoxUsbGlobal.hMonitor == INVALID_HANDLE_VALUE)
            {
                HRESULT hr = VBoxDrvCfgSvcStart(USBMON_SERVICE_NAME_W);
                if (hr == S_OK)
                {
                    g_VBoxUsbGlobal.hMonitor = CreateFile(USBMON_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_SYSTEM, NULL);
                    if (g_VBoxUsbGlobal.hMonitor == INVALID_HANDLE_VALUE)
                    {
                        DWORD winEr = GetLastError();
                        LogRel((__FUNCTION__": CreateFile failed winEr(%d)\n", winEr));
                        rc = VERR_FILE_NOT_FOUND;
                    }
                }
            }

            if (g_VBoxUsbGlobal.hMonitor != INVALID_HANDLE_VALUE)
            {
                USBSUP_VERSION Version = {0};
                DWORD cbReturned = 0;

                if (DeviceIoControl(g_VBoxUsbGlobal.hMonitor, SUPUSBFLT_IOCTL_GET_VERSION, NULL, 0, &Version, sizeof (Version), &cbReturned, NULL))
                {
                    if (Version.u32Major == USBMON_MAJOR_VERSION || Version.u32Minor <= USBMON_MINOR_VERSION)
                    {
#ifndef VBOX_USB_USE_DEVICE_NOTIFICATION
                        USBSUP_SET_NOTIFY_EVENT SetEvent = {0};
                        Assert(g_VBoxUsbGlobal.hNotifyEvent);
                        SetEvent.u.hEvent = g_VBoxUsbGlobal.hNotifyEvent;
                        if (DeviceIoControl(g_VBoxUsbGlobal.hMonitor, SUPUSBFLT_IOCTL_SET_NOTIFY_EVENT,
                                &SetEvent, sizeof (SetEvent),
                                &SetEvent, sizeof (SetEvent),
                                &cbReturned, NULL))
                        {
                            rc = SetEvent.u.rc;
                            AssertRC(rc);
                            if (RT_SUCCESS(rc))
                                return VINF_SUCCESS;
                            else
                                AssertMsgFailed((__FUNCTION__": SetEvent failed, rc (%d)\n", rc));
                        }
                        else
                        {
                            DWORD winEr = GetLastError();
                            AssertMsgFailed((__FUNCTION__": SetEvent Ioctl failed, winEr (%d)\n", winEr));
                            rc = VERR_VERSION_MISMATCH;
                        }
#else
                        g_VBoxUsbGlobal.hTimerQueue = CreateTimerQueue();
                        if (g_VBoxUsbGlobal.hTimerQueue)
                        {
                            g_VBoxUsbGlobal.hThread = CreateThread(
                              NULL, /*__in_opt   LPSECURITY_ATTRIBUTES lpThreadAttributes, */
                              0, /*__in       SIZE_T dwStackSize, */
                              usbLibMsgThreadProc, /*__in       LPTHREAD_START_ROUTINE lpStartAddress,*/
                              NULL, /*__in_opt   LPVOID lpParameter,*/
                              0, /*__in       DWORD dwCreationFlags,*/
                              NULL /*__out_opt  LPDWORD lpThreadId*/
                            );

                            if(g_VBoxUsbGlobal.hThread)
                            {
                                DWORD dwResult = WaitForSingleObject(g_VBoxUsbGlobal.hNotifyEvent, INFINITE);
                                Assert(dwResult == WAIT_OBJECT_0);

                                if (g_VBoxUsbGlobal.hWnd)
                                {
                                    /* ensure the event is set so the first "wait change" request processes */
                                    SetEvent(g_VBoxUsbGlobal.hNotifyEvent);
                                    return VINF_SUCCESS;
                                }
                                dwResult = WaitForSingleObject(g_VBoxUsbGlobal.hThread, INFINITE);
                                Assert(dwResult == WAIT_OBJECT_0);
                                BOOL bRc = CloseHandle(g_VBoxUsbGlobal.hThread);
                                if (!bRc)
                                {
                                    DWORD winEr = GetLastError();
                                    AssertMsgFailed((__FUNCTION__": CloseHandle for hThread failed winEr(%d)\n", winEr));
                                }
                            }
                            else
                            {
                                DWORD winEr = GetLastError();
                                AssertMsgFailed((__FUNCTION__": CreateThread failed, winEr (%d)\n", winEr));
                                rc = VERR_GENERAL_FAILURE;
                            }
                        }
                        else
                        {
                            DWORD winEr = GetLastError();
                            AssertMsgFailed((__FUNCTION__": CreateTimerQueue failed winEr(%d)\n", winEr));
                        }
#endif
                    }
                    else
                    {
                        AssertMsgFailed((__FUNCTION__": Monitor driver version mismatch!!\n"));
                        rc = VERR_VERSION_MISMATCH;
                    }
                }
                else
                {
                    DWORD winEr = GetLastError();
                    AssertMsgFailed((__FUNCTION__": DeviceIoControl failed winEr(%d)\n", winEr));
                    rc = VERR_VERSION_MISMATCH;
                }

                CloseHandle(g_VBoxUsbGlobal.hMonitor);
            }
            else
            {
                LogRel((__FUNCTION__": USB Service not found\n"));
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
                AssertFailed();
#endif
                rc = VERR_FILE_NOT_FOUND;
            }

            CloseHandle(g_VBoxUsbGlobal.hInterruptEvent);
        }
        else
        {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": CreateEvent for InterruptEvent failed winEr(%d)\n", winEr));
            rc = VERR_GENERAL_FAILURE;
        }

        CloseHandle(g_VBoxUsbGlobal.hNotifyEvent);
    }
    else
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CreateEvent for NotifyEvent failed winEr(%d)\n", winEr));
        rc = VERR_GENERAL_FAILURE;
    }

    /* since main calls us even if USBLibInit fails,
     * we use hMonitor == INVALID_HANDLE_VALUE as a marker to indicate whether the lib is inited */

    Assert(RT_FAILURE(rc));
    return rc;
}


/**
 * Terminate the USB library
 *
 * @returns VBox status code.
 */
USBLIB_DECL(int) USBLibTerm(void)
{
    if (g_VBoxUsbGlobal.hMonitor == INVALID_HANDLE_VALUE)
    {
#ifdef VBOX_WITH_ANNOYING_USB_ASSERTIONS
        AssertFailed();
#endif
        return VINF_ALREADY_INITIALIZED;
    }

    BOOL bRc;
#ifdef VBOX_USB_USE_DEVICE_NOTIFICATION
    bRc= PostMessage(g_VBoxUsbGlobal.hWnd, WM_QUIT, 0, 0);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": PostMessage for hWnd failed winEr(%d)\n", winEr));
    }

    DWORD dwResult = WaitForSingleObject(g_VBoxUsbGlobal.hThread, INFINITE);
    Assert(dwResult == WAIT_OBJECT_0);
    bRc = CloseHandle(g_VBoxUsbGlobal.hThread);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CloseHandle for hThread failed winEr(%d)\n", winEr));
    }

    if (g_VBoxUsbGlobal.hTimer)
    {
        bRc = DeleteTimerQueueTimer(g_VBoxUsbGlobal.hTimerQueue, g_VBoxUsbGlobal.hTimer,
                INVALID_HANDLE_VALUE /* <-- to block until the timer is completed */
                            );
        if (!bRc)
        {
            DWORD winEr = GetLastError();
            AssertMsgFailed((__FUNCTION__": DeleteTimerQueueEx failed winEr(%d)\n", winEr));
        }
    }

    bRc = DeleteTimerQueueEx(g_VBoxUsbGlobal.hTimerQueue,
            INVALID_HANDLE_VALUE /* <-- to block until all timers are completed */
            );
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": DeleteTimerQueueEx failed winEr(%d)\n", winEr));
    }
#endif

    bRc = CloseHandle(g_VBoxUsbGlobal.hMonitor);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CloseHandle for hMonitor failed winEr(%d)\n", winEr));
    }

    bRc = CloseHandle(g_VBoxUsbGlobal.hInterruptEvent);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CloseHandle for hInterruptEvent failed winEr(%d)\n", winEr));
    }

    bRc = CloseHandle(g_VBoxUsbGlobal.hNotifyEvent);
    if (!bRc)
    {
        DWORD winEr = GetLastError();
        AssertMsgFailed((__FUNCTION__": CloseHandle for hNotifyEvent failed winEr(%d)\n", winEr));
    }

    return VINF_SUCCESS;
}
