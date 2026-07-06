/******************************************************************************
 *  Copyright 2021 NXP
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#include <errno.h>
#include <fcntl.h>
#ifdef ANDROID
#include <hardware/nfc.h>
#endif

#ifdef USE_LIBGPIOD
#include <gpiod.h>
#include <time.h>
#endif

#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <NfccI2cTransport.h>
#include <NfccAltTransport.h>
#include <phNfcStatus.h>
#include <phNxpLog.h>
#include <string.h>
#include "phNxpNciHal_utils.h"

#define CRC_LEN 2
#define NORMAL_MODE_HEADER_LEN 3
#define FW_DNLD_HEADER_LEN 2
#define FW_DNLD_LEN_OFFSET 1
#define NORMAL_MODE_LEN_OFFSET 2
#define FRAGMENTSIZE_MAX PHNFC_I2C_FRAGMENT_SIZE
extern phTmlNfc_i2cfragmentation_t fragmentation_enabled;
extern phTmlNfc_Context_t* gpphTmlNfc_Context;

NfccAltTransport::NfccAltTransport() {
#ifdef USE_LIBGPIOD
  gpio_chip = nullptr;
#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  line_req_ven = nullptr;
  line_req_fwdnld = nullptr;
  line_req_irq = nullptr;
  event_buffer = nullptr;
#else
  line_ven = nullptr;
  line_fwdnld = nullptr;
  line_irq = nullptr;
#endif
#else
  iEnableFd = 0;
  iInterruptFd = 0;
  iFwDnldFd = 0;
#endif
}

NfccAltTransport::~NfccAltTransport() {
#ifdef USE_LIBGPIOD
  ReleaseGpioLines();
#else
  if (iEnableFd > 0) close(iEnableFd);
  if (iInterruptFd > 0) close(iInterruptFd);
  if (iFwDnldFd > 0) close(iFwDnldFd);
#endif
}

#ifdef USE_LIBGPIOD
int NfccAltTransport::InitGpioLines() {
  NXPLOG_TML_D("%s Enter", __func__);
  char chip_path[64];

  snprintf(chip_path, sizeof(chip_path), "/dev/%s", GPIO_CHIP_NAME);
  gpio_chip = gpiod_chip_open(chip_path);
  if (!gpio_chip) {
    NXPLOG_TML_E("%s: Failed to open GPIO chip %s (%s)", __func__, GPIO_CHIP_NAME,
                 strerror(errno));
    return -1;
  }

#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  struct gpiod_line_settings *settings = nullptr;
  struct gpiod_line_config *line_cfg = nullptr;
  struct gpiod_request_config *req_cfg = nullptr;
  const unsigned int ven_offset[] = {PIN_ENABLE};
  const unsigned int fwdnld_offset[] = {PIN_FWDNLD};
  const unsigned int irq_offset[] = {PIN_INT};

  req_cfg = gpiod_request_config_new();
  if (!req_cfg) {
    NXPLOG_TML_E("%s: Failed to create request config", __func__);
    goto error;
  }
  gpiod_request_config_set_consumer(req_cfg, GPIO_CONSUMER_NAME);

  settings = gpiod_line_settings_new();
  if (!settings) goto error;
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
  gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);

  line_cfg = gpiod_line_config_new();
  if (!line_cfg) goto error;
  gpiod_line_config_add_line_settings(line_cfg, ven_offset, 1, settings);

  line_req_ven = gpiod_chip_request_lines(gpio_chip, req_cfg, line_cfg);
  if (!line_req_ven) {
    NXPLOG_TML_E("%s: Failed to request VEN line (pin %d): %s", __func__, PIN_ENABLE,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: VEN line (pin %d) configured as output", __func__, PIN_ENABLE);
  gpiod_line_config_free(line_cfg);
  line_cfg = nullptr;

  line_cfg = gpiod_line_config_new();
  if (!line_cfg) goto error;
  gpiod_line_config_add_line_settings(line_cfg, fwdnld_offset, 1, settings);

  line_req_fwdnld = gpiod_chip_request_lines(gpio_chip, req_cfg, line_cfg);
  if (!line_req_fwdnld) {
    NXPLOG_TML_E("%s: Failed to request FWDNLD line (pin %d): %s", __func__, PIN_FWDNLD,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: FWDNLD line (pin %d) configured as output", __func__, PIN_FWDNLD);
  gpiod_line_config_free(line_cfg);
  gpiod_line_settings_free(settings);
  line_cfg = nullptr;
  settings = nullptr;

  settings = gpiod_line_settings_new();
  if (!settings) goto error;
  gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
  gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_RISING);

  line_cfg = gpiod_line_config_new();
  if (!line_cfg) goto error;
  gpiod_line_config_add_line_settings(line_cfg, irq_offset, 1, settings);

  line_req_irq = gpiod_chip_request_lines(gpio_chip, req_cfg, line_cfg);
  if (!line_req_irq) {
    NXPLOG_TML_E("%s: Failed to request IRQ line (pin %d): %s", __func__, PIN_INT,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: IRQ line (pin %d) configured for rising edge", __func__, PIN_INT);

  event_buffer = gpiod_edge_event_buffer_new(1);
  if (!event_buffer) {
    NXPLOG_TML_E("%s: Failed to create event buffer", __func__);
    goto error;
  }

  gpiod_line_config_free(line_cfg);
  gpiod_line_settings_free(settings);
  gpiod_request_config_free(req_cfg);
#else
  line_ven = gpiod_chip_get_line(gpio_chip, PIN_ENABLE);
  if (!line_ven) {
    NXPLOG_TML_E("%s: Failed to get VEN line (pin %d)", __func__, PIN_ENABLE);
    goto error;
  }
  if (gpiod_line_request_output(line_ven, GPIO_CONSUMER_NAME, 0) < 0) {
    NXPLOG_TML_E("%s: Failed to request VEN line (pin %d): %s", __func__, PIN_ENABLE,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: VEN line (pin %d) configured as output", __func__, PIN_ENABLE);

  line_fwdnld = gpiod_chip_get_line(gpio_chip, PIN_FWDNLD);
  if (!line_fwdnld) {
    NXPLOG_TML_E("%s: Failed to get FWDNLD line (pin %d)", __func__, PIN_FWDNLD);
    goto error;
  }
  if (gpiod_line_request_output(line_fwdnld, GPIO_CONSUMER_NAME, 0) < 0) {
    NXPLOG_TML_E("%s: Failed to request FWDNLD line (pin %d): %s", __func__, PIN_FWDNLD,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: FWDNLD line (pin %d) configured as output", __func__, PIN_FWDNLD);

  line_irq = gpiod_chip_get_line(gpio_chip, PIN_INT);
  if (!line_irq) {
    NXPLOG_TML_E("%s: Failed to get IRQ line (pin %d)", __func__, PIN_INT);
    goto error;
  }
  if (gpiod_line_request_rising_edge_events(line_irq, GPIO_CONSUMER_NAME) < 0) {
    NXPLOG_TML_E("%s: Failed to request IRQ line (pin %d): %s", __func__, PIN_INT,
                 strerror(errno));
    goto error;
  }
  NXPLOG_TML_D("%s: IRQ line (pin %d) configured for rising edge", __func__, PIN_INT);
#endif

  NXPLOG_TML_D("%s: GPIO lines initialized successfully", __func__);
  return 0;

#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
error:
  if (line_cfg) gpiod_line_config_free(line_cfg);
  if (settings) gpiod_line_settings_free(settings);
  if (req_cfg) gpiod_request_config_free(req_cfg);
  ReleaseGpioLines();
  return -1;
#else
error:
  ReleaseGpioLines();
  return -1;
#endif
}

void NfccAltTransport::ReleaseGpioLines() {
  NXPLOG_TML_D("%s Enter", __func__);

#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  if (event_buffer) {
    gpiod_edge_event_buffer_free(event_buffer);
    event_buffer = nullptr;
  }
  if (line_req_ven) {
    gpiod_line_request_release(line_req_ven);
    line_req_ven = nullptr;
  }
  if (line_req_fwdnld) {
    gpiod_line_request_release(line_req_fwdnld);
    line_req_fwdnld = nullptr;
  }
  if (line_req_irq) {
    gpiod_line_request_release(line_req_irq);
    line_req_irq = nullptr;
  }
#else
  if (line_ven) {
    gpiod_line_release(line_ven);
    line_ven = nullptr;
  }
  if (line_fwdnld) {
    gpiod_line_release(line_fwdnld);
    line_fwdnld = nullptr;
  }
  if (line_irq) {
    gpiod_line_release(line_irq);
    line_irq = nullptr;
  }
#endif
  if (gpio_chip) {
    gpiod_chip_close(gpio_chip);
    gpio_chip = nullptr;
  }

  NXPLOG_TML_D("%s: GPIO resources released", __func__);
}
#endif /* USE_LIBGPIOD */

/*******************************************************************************
**
** Function         Flushdata
**
** Description      Reads payload of FW rsp from NFCC device into given buffer
**
** Parameters       pDevHandle - valid device handle
**                  pBuffer    - buffer for read data
**                  numRead    - number of bytes read by calling function
**
** Returns          always returns -1
**
*******************************************************************************/
int NfccAltTransport::Flushdata(void* pDevHandle, uint8_t* pBuffer,
                                int numRead) {
  int retRead = 0;
  uint16_t totalBtyesToRead =
      pBuffer[FW_DNLD_LEN_OFFSET] + FW_DNLD_HEADER_LEN + CRC_LEN;
  /* we shall read totalBtyesToRead-1 as one byte is already read by calling
   * function*/
  retRead = read((intptr_t)pDevHandle, pBuffer + numRead, totalBtyesToRead - 1);
  if (retRead > 0) {
    numRead += retRead;
    phNxpNciHal_print_packet("RECV", pBuffer, numRead);
  } else if (retRead == 0) {
    NXPLOG_TML_E("%s _i2c_read() [pyld] EOF", __func__);
  } else {
    if (bFwDnldFlag == false) {
      NXPLOG_TML_D("%s _i2c_read() [hdr] received", __func__);
      phNxpNciHal_print_packet("RECV", pBuffer - numRead,
                               NORMAL_MODE_HEADER_LEN);
    }
    NXPLOG_TML_E("%s _i2c_read() [pyld] errno : %x", __func__, errno);
  }
  SemPost();
  return -1;
}

/*******************************************************************************
**
** Function         Reset
**
** Description      Reset NFCC device, using VEN pin
**
** Parameters       pDevHandle     - valid device handle
**                  eType          - reset level
**
** Returns           0   - reset operation success
**                  -1   - reset operation failure
**
*******************************************************************************/
int NfccAltTransport::NfccReset(void* pDevHandle, NfccResetType eType) {
  int ret = -1;
  NXPLOG_TML_D("%s, VEN eType %ld", __func__, eType);

  if (NULL == pDevHandle) {
    return -1;
  }
  switch (eType) {
    case MODE_POWER_OFF:
      gpio_set_fwdl(0);
      gpio_set_ven(0);
      break;
    case MODE_POWER_ON:
      gpio_set_fwdl(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_DWNLD_WITH_VEN:
      gpio_set_fwdl(1);
      gpio_set_ven(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_DWND_HIGH:
      gpio_set_fwdl(1);
      break;
    case MODE_POWER_RESET:
      gpio_set_ven(0);
      gpio_set_ven(1);
      break;
    case MODE_FW_GPIO_LOW:
      gpio_set_fwdl(0);
      break;
    default:
      NXPLOG_TML_E("%s, VEN eType %ld", __func__, eType);
      return -1;
  }
  if ((eType != MODE_FW_DWNLD_WITH_VEN) && (eType != MODE_FW_DWND_HIGH)) {
    EnableFwDnldMode(false);
  }
  if ((eType == MODE_FW_DWNLD_WITH_VEN) || (eType == MODE_FW_DWND_HIGH)) {
    EnableFwDnldMode(true);
  }

  return ret;
}

/*******************************************************************************
**
** Function         GetNfcState
**
** Description      Get NFC state
**
** Parameters       pDevHandle     - valid device handle
** Returns           0   - unknown
**                   1   - FW DWL
**                   2 	 - NCI
**
*******************************************************************************/
int NfccAltTransport::GetNfcState(void* pDevHandle) {
  int ret = NFC_STATE_UNKNOWN;
  NXPLOG_TML_D("%s ", __func__);
  if (NULL == pDevHandle) {
    return ret;
  }
  ret = ioctl((intptr_t)pDevHandle, NFC_GET_NFC_STATE);
  NXPLOG_TML_D("%s :nfc state = %d", __func__, ret);
  return ret;
}
/*******************************************************************************
**
** Function         EnableFwDnldMode
**
** Description      updates the state to Download mode
**
** Parameters       True/False
**
** Returns          None
*******************************************************************************/
void NfccAltTransport::EnableFwDnldMode(bool mode) { bFwDnldFlag = mode; }

/*******************************************************************************
**
** Function         IsFwDnldModeEnabled
**
** Description      Returns the current mode
**
** Parameters       none
**
** Returns           Current mode download/NCI
*******************************************************************************/
bool_t NfccAltTransport::IsFwDnldModeEnabled(void) { return bFwDnldFlag; }

/*******************************************************************************
**
** Function         SemPost
**
** Description      sem_post 2c_read / write
**
** Parameters       none
**
** Returns          none
*******************************************************************************/
void NfccAltTransport::SemPost() {
  int sem_val = 0;
  sem_getvalue(&mTxRxSemaphore, &sem_val);
  if (sem_val == 0) {
    sem_post(&mTxRxSemaphore);
  }
}

/*******************************************************************************
**
** Function         SemTimedWait
**
** Description      Timed sem_wait for avoiding i2c_read & write overlap
**
** Parameters       none
**
** Returns          Sem_wait return status
*******************************************************************************/
int NfccAltTransport::SemTimedWait() {
  NFCSTATUS status = NFCSTATUS_FAILED;
  long sem_timedout = 500 * 1000 * 1000;
  int s = 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 0;
  ts.tv_nsec += sem_timedout;
  while ((s = sem_timedwait(&mTxRxSemaphore, &ts)) == -1 && errno == EINTR) {
    continue; /* Restart if interrupted by handler */
  }
  if (s != -1) {
    status = NFCSTATUS_SUCCESS;
  } else if (errno == ETIMEDOUT && s == -1) {
    NXPLOG_TML_E("%s :timed out errno = 0x%x", __func__, errno);
  }
  return status;
}

/*******************************************************************************
**
** Function         GetIrqState
**
** Description      Get state of IRQ GPIO
**
** Parameters       pDevHandle - valid device handle
**
** Returns          The state of IRQ line i.e. +ve if read is pending else Zer0.
**                  In the case of IOCTL error, it returns -ve value.
**
*******************************************************************************/
int NfccAltTransport::GetIrqState(void* pDevHandle) {
  int ret = -1;
  (void)pDevHandle;

  NXPLOG_TML_D("%s Enter", __func__);

#ifdef USE_LIBGPIOD
#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  if (!line_req_irq) {
    NXPLOG_TML_E("%s: IRQ line not initialized", __func__);
    return -1;
  }

  enum gpiod_line_value value = gpiod_line_request_get_value(line_req_irq, PIN_INT);
  if (value == GPIOD_LINE_VALUE_ERROR) {
    NXPLOG_TML_E("%s: Failed to read IRQ line (%s)", __func__, strerror(errno));
    return -1;
  }
  ret = (value == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
#else
  if (!line_irq) {
    NXPLOG_TML_E("%s: IRQ line not initialized", __func__);
    return -1;
  }

  int value = gpiod_line_get_value(line_irq);
  if (value < 0) {
    NXPLOG_TML_E("%s: Failed to read IRQ line (%s)", __func__, strerror(errno));
    return -1;
  }
  ret = value ? 1 : 0;
#endif

  NXPLOG_TML_D("%s exit: state = %d", __func__, ret);
  return ret;
#else /* sysfs implementation */

  int len;
  char buf[2];

  if (iInterruptFd <= 0) {
    NXPLOG_TML_E("Error with interrupt-detect pin (%d)", iInterruptFd);
    return (-1);
  }

  lseek(iInterruptFd, SEEK_SET, 0);

  len = read(iInterruptFd, buf, 2);

  if (len != 2) {
    NXPLOG_TML_E("Error with interrupt-detect pin (%s)", strerror(errno));
    return (0);
  }

  NXPLOG_TML_D("%s exit: state = %d", __func__, (buf[0] != '0'));
  return (buf[0] != '0');
#endif /* USE_LIBGPIOD */
}

#ifndef USE_LIBGPIOD
int NfccAltTransport::verifyPin(int pin, int isoutput, int edge) {
  char buf[40];
  int hasGpio = 0;
  NXPLOG_TML_D("%s Enter", __func__);
  sprintf(buf, "/sys/class/gpio/gpio%d", pin);
  NXPLOG_TML_D("Pin %s\n", buf);
  int fd = open(buf, O_RDONLY);
  if (fd <= 0) {
    NXPLOG_TML_D("Create pin %s\n", buf);
    if ((fd = open("/sys/class/gpio/export", O_WRONLY)) > 0) {
      sprintf(buf, "%d", pin);
      if (write(fd, buf, strlen(buf)) == strlen(buf)) {
        hasGpio = 1;
        usleep(100 * 1000);
      }
    } else {
      NXPLOG_TML_E("open failed for /sys/class/gpio/export\n");
      return -1;
    }
  } else {
    NXPLOG_TML_E("System already has pin %s\n", buf);
    hasGpio = 1;
  }
  close(fd);

  if (hasGpio) {
    sprintf(buf, "/sys/class/gpio/gpio%d/direction", pin);
    NXPLOG_TML_D("Direction %s\n", buf);
    fd = open(buf, O_WRONLY);
    if (fd <= 0) {
      NXPLOG_TML_E("Could not open direction port '%s' (%s)", buf,
                   strerror(errno));
      return -1;
    } else {
      if (isoutput) {
        if (write(fd, "out", 3) == 3) {
          NXPLOG_TML_D("Pin %d now an output\n", pin);
        }
        close(fd);

        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        fd = open(buf, O_RDWR);
        if (fd <= 0) {
        }
        close(fd);

        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        fd = open(buf, O_RDWR);
        if (fd <= 0) {
          NXPLOG_TML_E("Could not open value port '%s' (%s)", buf,
                       strerror(errno));
          return -1;
        } else {
          if (write(fd, "0", 1) == 1) {
            NXPLOG_TML_D("Pin %d now off\n", pin);
          }
          return (fd);
        }
      } else {
        if (write(fd, "in", 2) == 2) {
          NXPLOG_TML_D("Pin %d now an input\n", pin);
        }
        close(fd);

        if (edge != EDGE_NONE) {
          sprintf(buf, "/sys/class/gpio/gpio%d/edge", pin);
          NXPLOG_TML_D("Edge %s\n", buf);
          fd = open(buf, O_RDWR);
          if (fd <= 0) {
            NXPLOG_TML_E("Could not open edge port '%s' (%s)", buf,
                         strerror(errno));
            return -1;
          } else {
            char* edge_str = "none";
            switch (edge) {
              case EDGE_RISING:
                edge_str = "rising";
                break;
              case EDGE_FALLING:
                edge_str = "falling";
                break;
              case EDGE_BOTH:
                edge_str = "both";
                break;
            }
            int l = strlen(edge_str);
            NXPLOG_TML_D("Edge-string %s - %d\n", edge_str, l);
            if (write(fd, edge_str, l) == l) {
              NXPLOG_TML_D("Pin %d trigger on %s\n", pin, edge_str);
            }
            close(fd);
          }
        }

        sprintf(buf, "/sys/class/gpio/gpio%d/value", pin);
        NXPLOG_TML_D("Value %s\n", buf);
        fd = open(buf, O_RDONLY);
        if (fd <= 0) {
          NXPLOG_TML_E("Could not open value port '%s' (%s)", buf,
                       strerror(errno));
          return -1;
        } else {
          return (fd);
        }
      }
    }
  }
  return (0);
}
#endif /* !USE_LIBGPIOD */

void NfccAltTransport::gpio_set_ven(int value) {
#ifdef USE_LIBGPIOD
#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  if (line_req_ven) {
    enum gpiod_line_value val = value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(line_req_ven, PIN_ENABLE, val);
    NXPLOG_TML_D("%s: VEN set to %d", __func__, value);
    usleep(10 * 1000);
  }
#else
  if (line_ven) {
    gpiod_line_set_value(line_ven, value ? 1 : 0);
    NXPLOG_TML_D("%s: VEN set to %d", __func__, value);
    usleep(10 * 1000);
  }
#endif
#else
  if (iEnableFd > 0) {
    if (value == 0) {
      write(iEnableFd, "0", 1);
    } else {
      write(iEnableFd, "1", 1);
    }
    usleep(10 * 1000);
  }
#endif /* USE_LIBGPIOD */
}

void NfccAltTransport::gpio_set_fwdl(int value) {
#ifdef USE_LIBGPIOD
#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  if (line_req_fwdnld) {
    enum gpiod_line_value val = value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE;
    gpiod_line_request_set_value(line_req_fwdnld, PIN_FWDNLD, val);
    NXPLOG_TML_D("%s: FWDNLD set to %d", __func__, value);
    usleep(10 * 1000);
  }
#else
  if (line_fwdnld) {
    gpiod_line_set_value(line_fwdnld, value ? 1 : 0);
    NXPLOG_TML_D("%s: FWDNLD set to %d", __func__, value);
    usleep(10 * 1000);
  }
#endif
#else
  if (iFwDnldFd > 0) {
    if (value == 0) {
      write(iFwDnldFd, "0", 1);
    } else {
      write(iFwDnldFd, "1", 1);
    }
    usleep(10 * 1000);
  }
#endif /* USE_LIBGPIOD */
}

void NfccAltTransport::wait4interrupt(void) {
#ifdef USE_LIBGPIOD
#if defined(GPIOD_VERSION_MAJOR) && (GPIOD_VERSION_MAJOR >= 2)
  NXPLOG_TML_D("%s Enter (libgpiod v2)", __func__);

  if (!line_req_irq) {
    NXPLOG_TML_E("%s: IRQ line not initialized", __func__);
    return;
  }

  for (;;) {
    enum gpiod_line_value value =
        gpiod_line_request_get_value(line_req_irq, PIN_INT);
    if (value == GPIOD_LINE_VALUE_ERROR) {
      /* A read error must not be mistaken for "IRQ active". */
      NXPLOG_TML_E("%s: Failed to read IRQ line (%s)", __func__,
                   strerror(errno));
      break;
    }
    if (value == GPIOD_LINE_VALUE_ACTIVE) break;
    int ret = gpiod_line_request_wait_edge_events(line_req_irq, 1000000000LL);
    if (ret < 0) {
      NXPLOG_TML_E("%s: wait_edge_events failed (%s)", __func__, strerror(errno));
      break;
    } else if (ret == 0) {
      NXPLOG_TML_D("%s: wait timeout, retrying...", __func__);
      continue;
    } else {
      gpiod_line_request_read_edge_events(line_req_irq, event_buffer, 1);
      break;
    }
  }
#else
  NXPLOG_TML_D("%s Enter (libgpiod v1)", __func__);

  if (!line_irq) {
    NXPLOG_TML_E("%s: IRQ line not initialized", __func__);
    return;
  }

  for (;;) {
    int value = gpiod_line_get_value(line_irq);
    if (value < 0) {
      /* A read error must not be mistaken for "IRQ active". */
      NXPLOG_TML_E("%s: Failed to read IRQ line (%s)", __func__,
                   strerror(errno));
      break;
    }
    if (value > 0) break;
    struct timespec timeout;
    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;
    int ret = gpiod_line_event_wait(line_irq, &timeout);
    if (ret < 0) {
      NXPLOG_TML_E("%s: event_wait failed (%s)", __func__, strerror(errno));
      break;
    } else if (ret == 0) {
      NXPLOG_TML_D("%s: wait timeout, retrying...", __func__);
      continue;
    } else {
      struct gpiod_line_event event;
      gpiod_line_event_read(line_irq, &event);
      break;
    }
  }
#endif

  NXPLOG_TML_D("%s Exit", __func__);
#else
  struct pollfd fds[1];
  fds[0].fd = iInterruptFd;
  fds[0].events = POLLPRI;
  int timeout_msecs = -1;
  int ret;
  while (!GetIrqState(NULL)) {
    ret = poll(fds, 1, timeout_msecs);
    if (ret != 1) {
      NXPLOG_TML_D("wait4interrupt() %d - %s, ", ret, strerror(errno));
    }
  }
#endif /* USE_LIBGPIOD */
}

int NfccAltTransport::ConfigurePin()
{
#ifdef USE_LIBGPIOD
  if (InitGpioLines() < 0) {
    return NFCSTATUS_INVALID_DEVICE;
  }
#else
  iInterruptFd = verifyPin(PIN_INT, 0, EDGE_RISING);
  if (iInterruptFd < 0) return (NFCSTATUS_INVALID_DEVICE);
  iEnableFd = verifyPin(PIN_ENABLE, 1, EDGE_NONE);
  if (iEnableFd < 0) return (NFCSTATUS_INVALID_DEVICE);
  iFwDnldFd = verifyPin(PIN_FWDNLD, 1, EDGE_NONE);
  if (iFwDnldFd < 0) return (NFCSTATUS_INVALID_DEVICE);
#endif /* USE_LIBGPIOD */
  return NFCSTATUS_SUCCESS;
}
