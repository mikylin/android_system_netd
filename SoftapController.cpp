/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <linux/wireless.h>

#include <openssl/evp.h>
#include <openssl/sha.h>

#define LOG_TAG "SoftapController"
#include <cutils/log.h>
#include <netutils/ifc.h>
#include <private/android_filesystem_config.h>
#include "wifi.h"
#include "ResponseCode.h"

#include "SoftapController.h"

#ifndef HOSTAPD_DRIVER_NAME
#define HOSTAPD_DRIVER_NAME "nl80211"
#endif

static const char HOSTAPD_CONF_FILE[]    = "/data/misc/wifi/hostapd.conf";
static const char HOSTAPD_BIN_FILE[]    = "/system/bin/hostapd";

SoftapController::SoftapController()
    : mPid(0) {
    mSock = socket(AF_INET, SOCK_DGRAM, 0);
    if (mSock < 0)
        ALOGE("Failed to open socket");
    memset(mIface, 0, sizeof(mIface));
}

SoftapController::~SoftapController() {
    if (mSock >= 0)
        close(mSock);
}

int SoftapController::setCommand(char *iface, const char *fname, unsigned buflen) {
#ifdef HAVE_HOSTAPD
    return 0;
#else
    char tBuf[SOFTAP_MAX_BUFFER_SIZE];
    struct iwreq wrq;
    struct iw_priv_args *priv_ptr;
    int i, j, ret;
    int cmd = 0, sub_cmd = 0;
   
    strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
    wrq.u.data.pointer = tBuf;
    wrq.u.data.length = sizeof(tBuf) / sizeof(struct iw_priv_args);
    wrq.u.data.flags = 0;
    if ((ret = ioctl(mSock, SIOCGIWPRIV, &wrq)) < 0) {
        ALOGE("SIOCGIPRIV failed: %d", ret);
        return ret;
   }
   
   priv_ptr = (struct iw_priv_args *)wrq.u.data.pointer;
   for(i=0; i < wrq.u.data.length;i++) {
       if (strcmp(priv_ptr[i].name, fname) == 0) {
           cmd = priv_ptr[i].cmd;
           break;
       }
   }

   if (i == wrq.u.data.length) {
       ALOGE("iface:%s, fname: %s - function not supported", iface, fname);
       return -1;
   }

  if (cmd < SIOCDEVPRIVATE) {
      for(j=0; j < i; j++) {
          if ((priv_ptr[j].set_args == priv_ptr[i].set_args) &&
              (priv_ptr[j].get_args == priv_ptr[i].get_args) &&
              (priv_ptr[j].name[0] == '\0'))
              break;
      }
      if (j == i) {
          ALOGE("iface:%s, fname: %s - invalid private ioctl", iface, fname);
          return -1;
      }
      sub_cmd = cmd;
      cmd = priv_ptr[j].cmd;

  }
  strncpy(wrq.ifr_name, iface, sizeof(wrq.ifr_name));
  if ((buflen == 0) && (*mBuf != 0))
       wrq.u.data.length = strlen(mBuf) + 1;
  else
      wrq.u.data.length = buflen;
  wrq.u.data.pointer = mBuf;
  wrq.u.data.flags = sub_cmd;
  ret = ioctl(mSock, cmd, &wrq);
  return ret;
#endif
}

int SoftapController::startDriver(char *iface) {
    int ret;

    if (mSock < 0) {
        ALOGE("Softap driver start - failed to open socket");
        return ResponseCode::OperationFailed;
    }
    if (!iface || (iface[0] == '\0')) {
        ALOGD("Softap driver start - wrong interface");
        iface = mIface;
     }

    *mBuf = 0;
    ret = setCommand(iface, "START");
    if (ret < 0) {
        ALOGE("Softap driver start: %d", ret);
        return ResponseCode::ServiceStartFailed;
     }
#ifdef HAVE_HOSTAPD
    ifc_init();
    ret = ifc_up(iface);
    ifc_close();
#endif
    usleep(AP_DRIVER_START_DELAY);
    ALOGD("Softap driver start: %d", ret);
    return ResponseCode::SoftapStatusResult;
}

int SoftapController::stopDriver(char *iface) {
    int ret;

    if (mSock < 0) {
        ALOGE("Softap driver stop - failed to open socket");
        return ResponseCode::OperationFailed;
    }
    if (!iface || (iface[0] == '\0')) {
        ALOGD("Softap driver stop - wrong interface");
        iface = mIface;
    }
    *mBuf = 0;
#ifdef HAVE_HOSTAPD
    ifc_init();
    ret = ifc_down(iface);
    ifc_close();
    if (ret < 0) {
        ALOGE("Softap %s down: %d", iface, ret);
    }
#endif
    ret = setCommand(iface, "STOP");
    ALOGD("Softap driver stop: %d", ret);
    return ResponseCode::SoftapStatusResult;
}

int SoftapController::startSoftap() {
    pid_t pid = 1;
    int ret = 0;

    if (mPid) {
        ALOGE("SoftAP is already running");
        return ResponseCode::SoftapStatusResult;
    }
    if (mSock < 0) {
        ALOGE("Softap startap - failed to open socket");
        return ResponseCode::OperationFailed;
    }
#ifdef HAVE_HOSTAPD
    if ((pid = fork()) < 0) {
        ALOGE("fork failed (%s)", strerror(errno));
        return ResponseCode::ServiceStartFailed;
    }
#endif
    if (!pid) {
#ifdef HAVE_HOSTAPD
#ifndef HOSTAPD_NO_ENTROPY
        ensure_entropy_file_exists();
#endif
        if (execl(HOSTAPD_BIN_FILE, HOSTAPD_BIN_FILE,
#ifndef HOSTAPD_NO_ENTROPY
                  "-e", WIFI_ENTROPY_FILE,
#endif
                  HOSTAPD_CONF_FILE, (char *) NULL)) {
            ALOGE("execl failed (%s)", strerror(errno));
        }
#endif
        ALOGE("SoftAP failed to start");
        return ResponseCode::ServiceStartFailed;
    } else {
        *mBuf = 0;
        ret = setCommand(mIface, "AP_BSS_START");
         if (ret) {
             ALOGE("Softap startap - failed: %d", ret);
                         return ResponseCode::ServiceStartFailed;
          }
          else {
              mPid = pid;
              ALOGD("SoftAP started successfully");
              usleep(AP_BSS_START_DELAY);
          }
    }
    return ResponseCode::SoftapStatusResult;
}

int SoftapController::stopSoftap() {

    if (mPid == 0) {
        ALOGE("SoftAP is not running");
        return ResponseCode::SoftapStatusResult;
    }
 
#ifdef HAVE_HOSTAPD
    ALOGD("Stopping the SoftAP service...");
    kill(mPid, SIGTERM);
    waitpid(mPid, NULL, 0);
#endif
    if (mSock < 0) {
        ALOGE("Softap stopap - failed to open socket");
        return ResponseCode::OperationFailed;
    }
    *mBuf = 0;
    setCommand(mIface, "AP_BSS_STOP");
    mPid = 0;
    ALOGD("SoftAP stopped successfully");
    usleep(AP_BSS_STOP_DELAY);
    return ResponseCode::SoftapStatusResult;
}

bool SoftapController::isSoftapStarted() {
    return (mPid != 0);
}

int SoftapController::addParam(int pos, const char *cmd, const char *arg)
{
   if (pos < 0)
       return pos;
   if ((unsigned)(pos + strlen(cmd) + strlen(arg) + 1) >= sizeof(mBuf)) {
       ALOGE("Command line is too big");
       return -1;

   }
   pos += sprintf(&mBuf[pos], "%s=%s,", cmd, arg);
   return pos;
}

/*
 * Arguments:
 *  argv[2] - wlan interface
 *  argv[3] - softap interface
 *  argv[4] - SSID
 *  argv[5] - Broadcast/Hidden
 *  argv[6] - Channel
 *  argv[7] - Security
 *  argv[8] - Key
 * the following argv may missing
 *        argv[9] - Preamble
 *        argv[10] - Max SCB
 */
int SoftapController::setSoftap(int argc, char *argv[]) {
    char psk_str[2*SHA256_DIGEST_LENGTH+1];
    int ret = ResponseCode::SoftapStatusResult;
    int i = 0;
    char *ssid, *iface;
    int fd;
    int hidden = 0;
    int channel = AP_CHANNEL_DEFAULT;
    char *wbuf = NULL;
    char *fbuf = NULL;

    if (mSock < 0) {
         ALOGE("Softap set - failed to open socket");
         return ResponseCode::OperationFailed;
    }
    if (argc < 5) {
        ALOGE("Softap set is missing arguments. Please use:");
        ALOGE("softap <wlan iface> <SSID> <hidden/broadcast> <channel> <wpa2?-psk|open> <passphrase>");
        return ResponseCode::CommandSyntaxError;
    }

    strncpy(mIface, argv[3], sizeof(mIface));
    iface = argv[2];

#ifdef HAVE_HOSTAPD
     if (!strcasecmp(argv[5], "hidden"))
        hidden = 1;

     if (argc > 4) {
         ssid = argv[4];
     } else {
          ssid = (char *)"AndroidAP";
     }

    if (argc >= 5) {
        channel = atoi(argv[5]);
        if (channel <= 0)
            channel = AP_CHANNEL_DEFAULT;
    }

    asprintf(&wbuf, "interface=%s\ndriver=" HOSTAPD_DRIVER_NAME "\nctrl_interface="
            "/data/misc/wifi/hostapd\nssid=%s\nchannel=%d\nieee80211n=1\n"
            "hw_mode=g\nignore_broadcast_ssid=%d\n",
            iface, ssid, channel, hidden);

    if (argc > 7) {
        if (!strcmp(argv[7], "wpa-psk")) {
            generatePsk(ssid, argv[8], psk_str);
            asprintf(&fbuf, "%swpa=1\nwpa_pairwise=TKIP CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[7], "wpa2-psk")) {
            generatePsk(ssid, argv[8], psk_str);
            asprintf(&fbuf, "%swpa=2\nrsn_pairwise=CCMP\nwpa_psk=%s\n", wbuf, psk_str);
        } else if (!strcmp(argv[7], "open")) {
            asprintf(&fbuf, "%s", wbuf);
        }
    } else if (argc > 6) {
        if (!strcmp(argv[6], "open")) {
            asprintf(&fbuf, "%s", wbuf);
        }
    } else {
        asprintf(&fbuf, "%s", wbuf);
    }

    fd = open(HOSTAPD_CONF_FILE, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0660);
    if (fd < 0) {
        ALOGE("Cannot update \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        free(wbuf);
        free(fbuf);
        return ResponseCode::OperationFailed;
    }
    if (write(fd, fbuf, strlen(fbuf)) < 0) {
        ALOGE("Cannot write to \"%s\": %s", HOSTAPD_CONF_FILE, strerror(errno));
        ret = ResponseCode::OperationFailed;
    }
    free(wbuf);
    free(fbuf);

    /* Note: apparently open can fail to set permissions correctly at times */
    if (fchmod(fd, 0660) < 0) {
        ALOGE("Error changing permissions of %s to 0660: %s",
                HOSTAPD_CONF_FILE, strerror(errno));
        close(fd);
        unlink(HOSTAPD_CONF_FILE);
        return ResponseCode::OperationFailed;
    }

    if (fchown(fd, AID_SYSTEM, AID_WIFI) < 0) {
        ALOGE("Error changing group ownership of %s to %d: %s",
                HOSTAPD_CONF_FILE, AID_WIFI, strerror(errno));
        close(fd);
        unlink(HOSTAPD_CONF_FILE);
        return ResponseCode::OperationFailed;
    }
    close(fd);
#else
    /* Create command line */
    i = addParam(i, "ASCII_CMD", "AP_CFG");
    if (argc > 4) {
        ssid = argv[4];
    } else {
        ssid = (char *)"AndroidAP";
    }
    i = addParam(i, "SSID", ssid);
    if (argc > 7) {
        i = addParam(i, "SEC", argv[7]);
    } else {
        i = addParam(i, "SEC", "open");
    }
    if (argc > 8) {
        generatePsk(ssid, argv[8], psk_str);
        i = addParam(i, "KEY", psk_str);
    } else {
        i = addParam(i, "KEY", "12345678");
    }
    if (argc > 6) {
        i = addParam(i, "CHANNEL", argv[6]);
    } else {
        i = addParam(i, "CHANNEL", "6");
    }
    if (argc > 9) {
        i = addParam(i, "PREAMBLE", argv[9]);
    } else {
        i = addParam(i, "PREAMBLE", "0");
    }
    if (argc > 10) {
        i = addParam(i, "MAX_SCB", argv[10]);
    } else {
        i = addParam(i, "MAX_SCB", "8");
    }
if ((i < 0) || ((unsigned)(i + 4) >= sizeof(mBuf))) {
     ALOGE("Softap set - command is too big");
     return ResponseCode::OperationFailed;
    }
    sprintf(&mBuf[i], "END");

    /* system("iwpriv eth0 WL_AP_CFG ASCII_CMD=AP_CFG,SSID=\"AndroidAP\",SEC=\"open\",KEY=12345,CHANNEL=1,PREAMBLE=0,MAX_SCB=8,END"); */
    ret = setCommand(iface, "AP_SET_CFG");
    if (ret) {
        ALOGE("Softap set - failed: %d", ret);
                ret = ResponseCode::ServiceStartFailed;
    }
     else {
           ALOGD("Softap set - Ok");
           usleep(AP_SET_CFG_DELAY);
    }
#endif
    return ret;
}

/*
 * Arguments:
 *	argv[2] - interface name
 *	argv[3] - AP or P2P or STA
 */
int SoftapController::fwReloadSoftap(int argc, char *argv[])
{
    int i = 0;
    char *iface;
    char *fwpath = NULL;

    if (mSock < 0) {
        ALOGE("Softap fwrealod - failed to open socket");
        return ResponseCode::OperationFailed;
    }
    if (argc < 4) {
        ALOGE("SoftAP fwreload is missing arguments. Please use: softap <wlan iface> <AP|P2P|STA>");
        return ResponseCode::CommandSyntaxError;
    }

    iface = argv[2];

    if (strcmp(argv[3], "AP") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_AP);
    } else if (strcmp(argv[3], "P2P") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_P2P);
    } else if (strcmp(argv[3], "STA") == 0) {
        fwpath = (char *)wifi_get_fw_path(WIFI_GET_FW_PATH_STA);
    }
    if (!fwpath)
        return ResponseCode::CommandParameterError;
#ifdef HAVE_HOSTAPD
    if (wifi_change_fw_path((const char *)fwpath)) {
#else
    sprintf(mBuf, "FW_PATH=%s", fwpath);
    if (setCommand(iface, "WL_FW_RELOAD")) {
#endif
        ALOGE("Softap fwReload failed");
        return ResponseCode::OperationFailed;
    }
    else {
        ALOGD("Softap fwReload - Ok");
    }
    return ResponseCode::SoftapStatusResult;
}

void SoftapController::generatePsk(char *ssid, char *passphrase, char *psk_str) {
    unsigned char psk[SHA256_DIGEST_LENGTH];
    int j;
    // Use the PKCS#5 PBKDF2 with 4096 iterations
    PKCS5_PBKDF2_HMAC_SHA1(passphrase, strlen(passphrase),
            reinterpret_cast<const unsigned char *>(ssid), strlen(ssid),
            4096, SHA256_DIGEST_LENGTH, psk);
    for (j=0; j < SHA256_DIGEST_LENGTH; j++) {
        sprintf(&psk_str[j*2], "%02x", psk[j]);
    }
}
