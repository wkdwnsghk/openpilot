#include "health.h"

extern int _app_start[0xc000]; // Only first 3 sectors of size 0x4000 are used

// Prototypes
void set_safety_mode(uint16_t mode, uint16_t param);
bool is_car_safety_mode(uint16_t mode);

int get_health_pkt(void *dat) {
  COMPILE_TIME_ASSERT(sizeof(struct health_t) <= USBPACKET_MAX_SIZE);
  struct health_t * health = (struct health_t*)dat;

  health->uptime_pkt = uptime_cnt;
  health->voltage_pkt = adc_get_voltage();
  health->current_pkt = current_board->read_current();

  //Use the GPIO pin to determine ignition or use a CAN based logic
  health->ignition_line_pkt = (uint8_t)(current_board->check_ignition());
  health->ignition_can_pkt = (uint8_t)(ignition_can);

  health->controls_allowed_pkt = controls_allowed;
  health->gas_interceptor_detected_pkt = gas_interceptor_detected;
  health->can_rx_errs_pkt = can_rx_errs;
  health->can_send_errs_pkt = can_send_errs;
  health->can_fwd_errs_pkt = can_fwd_errs;
  health->gmlan_send_errs_pkt = gmlan_send_errs;
  health->car_harness_status_pkt = car_harness_status;
  health->usb_power_mode_pkt = usb_power_mode;
  health->safety_mode_pkt = (uint8_t)(current_safety_mode);
  health->safety_param_pkt = current_safety_param;
  health->alternative_experience_pkt = alternative_experience;
  health->power_save_enabled_pkt = (uint8_t)(power_save_status == POWER_SAVE_STATUS_ENABLED);
  health->heartbeat_lost_pkt = (uint8_t)(heartbeat_lost);
  health->blocked_msg_cnt_pkt = blocked_msg_cnt;

  health->fault_status_pkt = fault_status;
  health->faults_pkt = faults;

  health->interrupt_load = interrupt_load;

  return sizeof(*health);
}

int get_rtc_pkt(void *dat) {
  timestamp_t t = rtc_get_time();
  (void)memcpy(dat, &t, sizeof(t));
  return sizeof(t);
}

typedef struct {
  uint32_t ptr;
  uint32_t tail_size;
  uint8_t data[72];
  uint8_t counter;
} asm_buffer;

asm_buffer can_read_buffer = {.ptr = 0U, .tail_size = 0U, .counter = 0U};
uint32_t total_rx_size = 0U;

int comms_can_read(uint8_t *data, uint32_t max_len) {
  uint32_t pos = 1;
  data[0] = can_read_buffer.counter;
  // Send tail of previous message if it is in buffer
  if (can_read_buffer.ptr > 0U) {
    if (can_read_buffer.ptr <= 63U) {
      (void)memcpy(&data[pos], can_read_buffer.data, can_read_buffer.ptr);
      pos += can_read_buffer.ptr;
      can_read_buffer.ptr = 0U;
    } else {
      (void)memcpy(&data[pos], can_read_buffer.data, 63U);
      can_read_buffer.ptr = can_read_buffer.ptr - 63U;
      (void)memcpy(can_read_buffer.data, &can_read_buffer.data[63], can_read_buffer.ptr);
      pos += 63U;
    }
  }

  if (total_rx_size > MAX_EP1_CHUNK_PER_BULK_TRANSFER) {
    total_rx_size = 0U;
    can_read_buffer.counter = 0U;
  } else {
    CANPacket_t can_packet;
    while ((pos < max_len) && can_pop(&can_rx_q, &can_packet)) {
      uint32_t pckt_len = CANPACKET_HEAD_SIZE + dlc_to_len[can_packet.data_len_code];
      if ((pos + pckt_len) <= max_len) {
        (void)memcpy(&data[pos], &can_packet, pckt_len);
        pos += pckt_len;
      } else {
        (void)memcpy(&data[pos], &can_packet, max_len - pos);
        can_read_buffer.ptr = pckt_len - (max_len - pos);
        // cppcheck-suppress objectIndex
        (void)memcpy(can_read_buffer.data, &((uint8_t*)&can_packet)[(max_len - pos)], can_read_buffer.ptr);
        pos = max_len;
      }
    }
    can_read_buffer.counter++;
    total_rx_size += pos;
  }
  if (pos != max_len) {
    can_read_buffer.counter = 0U;
    total_rx_size = 0U;
  }
  if (pos <= 1U) { pos = 0U; }
  return pos;
}

asm_buffer can_write_buffer = {.ptr = 0U, .tail_size = 0U, .counter = 0U};

// send on CAN
void comms_can_write(uint8_t *data, uint32_t len) {
  // Got first packet from a stream, resetting buffer and counter
  if (data[0] == 0U) {
    can_write_buffer.counter = 0U;
    can_write_buffer.ptr = 0U;
    can_write_buffer.tail_size = 0U;
  }
  // Assembling can message with data from buffer
  if (data[0] == can_write_buffer.counter) {
    uint32_t pos = 1U;
    can_write_buffer.counter++;
    if (can_write_buffer.ptr != 0U) {
      if (can_write_buffer.tail_size <= 63U) {
        CANPacket_t to_push;
        (void)memcpy(&can_write_buffer.data[can_write_buffer.ptr], &data[pos], can_write_buffer.tail_size);
        (void)memcpy(&to_push, can_write_buffer.data, can_write_buffer.ptr + can_write_buffer.tail_size);
        can_send(&to_push, to_push.bus, false);
        pos += can_write_buffer.tail_size;
        can_write_buffer.ptr = 0U;
        can_write_buffer.tail_size = 0U;
      } else {
        (void)memcpy(&can_write_buffer.data[can_write_buffer.ptr], &data[pos], len - pos);
        can_write_buffer.tail_size -= 63U;
        can_write_buffer.ptr += 63U;
        pos += 63U;
      }
    }

    while (pos < len) {
      uint32_t pckt_len = CANPACKET_HEAD_SIZE + dlc_to_len[(data[pos] >> 4U)];
      if ((pos + pckt_len) <= len) {
        CANPacket_t to_push;
        (void)memcpy(&to_push, &data[pos], pckt_len);
        can_send(&to_push, to_push.bus, false);
        pos += pckt_len;
      } else {
        (void)memcpy(can_write_buffer.data, &data[pos], len - pos);
        can_write_buffer.ptr = len - pos;
        can_write_buffer.tail_size = pckt_len - can_write_buffer.ptr;
        pos += can_write_buffer.ptr;
      }
    }
  }
}

// send on serial, first byte to select the ring
void comms_endpoint2_write(uint8_t *data, uint32_t len) {
  uart_ring *ur = get_ring_by_number(data[0]);
  if ((len != 0U) && (ur != NULL)) {
    if ((data[0] < 2U) || safety_tx_lin_hook(data[0] - 2U, &data[1], len - 1U)) {
      for (uint32_t i = 1; i < len; i++) {
        while (!putc(ur, data[i])) {
          // wait
        }
      }
    }
  }
}

// TODO: make this more general!
void usb_cb_ep3_out_complete(void) {
  if (can_tx_check_min_slots_free(MAX_CAN_MSGS_PER_BULK_TRANSFER)) {
    usb_outep3_resume_if_paused();
  }
}

int comms_control_handler(ControlPacket_t *req, uint8_t *resp) {
  unsigned int resp_len = 0;
  uart_ring *ur = NULL;
  timestamp_t t;
  switch (req->request) {
    // **** 0xa0: get rtc time
    case 0xa0:
      resp_len = get_rtc_pkt(resp);
      break;
    // **** 0xa1: set rtc year
    case 0xa1:
      t = rtc_get_time();
      t.year = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa2: set rtc month
    case 0xa2:
      t = rtc_get_time();
      t.month = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa3: set rtc day
    case 0xa3:
      t = rtc_get_time();
      t.day = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa4: set rtc weekday
    case 0xa4:
      t = rtc_get_time();
      t.weekday = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa5: set rtc hour
    case 0xa5:
      t = rtc_get_time();
      t.hour = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa6: set rtc minute
    case 0xa6:
      t = rtc_get_time();
      t.minute = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xa7: set rtc second
    case 0xa7:
      t = rtc_get_time();
      t.second = req->param1;
      rtc_set_time(t);
      break;
    // **** 0xb0: set IR power
    case 0xb0:
      current_board->set_ir_power(req->param1);
      break;
    // **** 0xb1: set fan power
    case 0xb1:
      current_board->set_fan_power(req->param1);
      break;
    // **** 0xb2: get fan rpm
    case 0xb2:
      resp[0] = (fan_rpm & 0x00FFU);
      resp[1] = ((fan_rpm & 0xFF00U) >> 8U);
      resp_len = 2;
      break;
    // **** 0xb3: set phone power
    case 0xb3:
      current_board->set_phone_power(req->param1 > 0U);
      break;
    // **** 0xc0: get CAN debug info
    case 0xc0:
      puts("can tx: "); puth(can_tx_cnt);
      puts(" txd: "); puth(can_txd_cnt);
      puts(" rx: "); puth(can_rx_cnt);
      puts(" err: "); puth(can_err_cnt);
      puts("\n");
      break;
    // **** 0xc1: get hardware type
    case 0xc1:
      resp[0] = hw_type;
      resp_len = 1;
      break;
    // **** 0xd0: fetch serial number
    case 0xd0:
      // addresses are OTP
      if (req->param1 == 1U) {
        (void)memcpy(resp, (uint8_t *)DEVICE_SERIAL_NUMBER_ADDRESS, 0x10);
        resp_len = 0x10;
      } else {
        get_provision_chunk(resp);
        resp_len = PROVISION_CHUNK_LEN;
      }
      break;
    // **** 0xd1: enter bootloader mode
    case 0xd1:
      // this allows reflashing of the bootstub
      switch (req->param1) {
        case 0:
          // only allow bootloader entry on debug builds
          #ifdef ALLOW_DEBUG
            puts("-> entering bootloader\n");
            enter_bootloader_mode = ENTER_BOOTLOADER_MAGIC;
            NVIC_SystemReset();
          #endif
          break;
        case 1:
          puts("-> entering softloader\n");
          enter_bootloader_mode = ENTER_SOFTLOADER_MAGIC;
          NVIC_SystemReset();
          break;
        default:
          puts("Bootloader mode invalid\n");
          break;
      }
      break;
    // **** 0xd2: get health packet
    case 0xd2:
      resp_len = get_health_pkt(resp);
      break;
    // **** 0xd3: get first 64 bytes of signature
    case 0xd3:
      {
        resp_len = 64;
        char * code = (char*)_app_start;
        int code_len = _app_start[0];
        (void)memcpy(resp, &code[code_len], resp_len);
      }
      break;
    // **** 0xd4: get second 64 bytes of signature
    case 0xd4:
      {
        resp_len = 64;
        char * code = (char*)_app_start;
        int code_len = _app_start[0];
        (void)memcpy(resp, &code[code_len + 64], resp_len);
      }
      break;
    // **** 0xd6: get version
    case 0xd6:
      COMPILE_TIME_ASSERT(sizeof(gitversion) <= USBPACKET_MAX_SIZE);
      (void)memcpy(resp, gitversion, sizeof(gitversion));
      resp_len = sizeof(gitversion) - 1U;
      break;
    // **** 0xd8: reset ST
    case 0xd8:
      NVIC_SystemReset();
      break;
    // **** 0xd9: set ESP power
    case 0xd9:
      if (req->param1 == 1U) {
        current_board->set_gps_mode(GPS_ENABLED);
      } else if (req->param1 == 2U) {
        current_board->set_gps_mode(GPS_BOOTMODE);
      } else {
        current_board->set_gps_mode(GPS_DISABLED);
      }
      break;
    // **** 0xda: reset ESP, with optional boot mode
    case 0xda:
      current_board->set_gps_mode(GPS_DISABLED);
      delay(1000000);
      if (req->param1 == 1U) {
        current_board->set_gps_mode(GPS_BOOTMODE);
      } else {
        current_board->set_gps_mode(GPS_ENABLED);
      }
      delay(1000000);
      current_board->set_gps_mode(GPS_ENABLED);
      break;
    // **** 0xdb: set GMLAN (white/grey) or OBD CAN (black) multiplexing mode
    case 0xdb:
      if(current_board->has_obd){
        if (req->param1 == 1U) {
          // Enable OBD CAN
          current_board->set_can_mode(CAN_MODE_OBD_CAN2);
        } else {
          // Disable OBD CAN
          current_board->set_can_mode(CAN_MODE_NORMAL);
        }
      } else {
        if (req->param1 == 1U) {
          // GMLAN ON
          if (req->param2 == 1U) {
            can_set_gmlan(1);
          } else if (req->param2 == 2U) {
            can_set_gmlan(2);
          } else {
            puts("Invalid bus num for GMLAN CAN set\n");
          }
        } else {
          can_set_gmlan(-1);
        }
      }
      break;

    // **** 0xdc: set safety mode
    case 0xdc:
      set_safety_mode(req->param1, (uint16_t)req->param2);
      break;
    // **** 0xdd: get healthpacket and CANPacket versions
    case 0xdd:
      resp[0] = HEALTH_PACKET_VERSION;
      resp[1] = CAN_PACKET_VERSION;
      resp_len = 2;
      break;
    // **** 0xde: set can bitrate
    case 0xde:
      if (req->param1 < BUS_CNT) {
        // TODO: add sanity check, ideally check if value is correct(from array of correct values)
        bus_config[req->param1].can_speed = req->param2;
        bool ret = can_init(CAN_NUM_FROM_BUS_NUM(req->param1));
        UNUSED(ret);
      }
      break;
    // **** 0xdf: set alternative experience
    case 0xdf:
      // you can only set this if you are in a non car safety mode
      if (!is_car_safety_mode(current_safety_mode)) {
        alternative_experience = req->param1;
      }
      break;
    // **** 0xe0: uart read
    case 0xe0:
      ur = get_ring_by_number(req->param1);
      if (!ur) {
        break;
      }

      // TODO: Remove this again and fix boardd code to hande the message bursts instead of single chars
      if (ur == &uart_ring_gps) {
        dma_pointer_handler(ur, DMA2_Stream5->NDTR);
      }

      // read
      while ((resp_len < MIN(req->length, USBPACKET_MAX_SIZE)) &&
                         getc(ur, (char*)&resp[resp_len])) {
        ++resp_len;
      }
      break;
    // **** 0xe1: uart set baud rate
    case 0xe1:
      ur = get_ring_by_number(req->param1);
      if (!ur) {
        break;
      }
      uart_set_baud(ur->uart, req->param2);
      break;
    // **** 0xe2: uart set parity
    case 0xe2:
      ur = get_ring_by_number(req->param1);
      if (!ur) {
        break;
      }
      switch (req->param2) {
        case 0:
          // disable parity, 8-bit
          ur->uart->CR1 &= ~(USART_CR1_PCE | USART_CR1_M);
          break;
        case 1:
          // even parity, 9-bit
          ur->uart->CR1 &= ~USART_CR1_PS;
          ur->uart->CR1 |= USART_CR1_PCE | USART_CR1_M;
          break;
        case 2:
          // odd parity, 9-bit
          ur->uart->CR1 |= USART_CR1_PS;
          ur->uart->CR1 |= USART_CR1_PCE | USART_CR1_M;
          break;
        default:
          break;
      }
      break;
    // **** 0xe4: uart set baud rate extended
    case 0xe4:
      ur = get_ring_by_number(req->param1);
      if (!ur) {
        break;
      }
      uart_set_baud(ur->uart, (int)req->param2*300);
      break;
    // **** 0xe5: set CAN loopback (for testing)
    case 0xe5:
      can_loopback = (req->param1 > 0U);
      can_init_all();
      break;
    // **** 0xe6: set USB power
    case 0xe6:
      current_board->set_usb_power_mode(req->param1);
      break;
    // **** 0xe7: set power save state
    case 0xe7:
      set_power_save_state(req->param1);
      break;
    // **** 0xf0: k-line/l-line wake-up pulse for KWP2000 fast initialization
    case 0xf0:
      if(current_board->has_lin) {
        bool k = (req->param1 == 0U) || (req->param1 == 2U);
        bool l = (req->param1 == 1U) || (req->param1 == 2U);
        if (bitbang_wakeup(k, l)) {
          resp_len = -1; // do not clear NAK yet (wait for bit banging to finish)
        }
      }
      break;
    // **** 0xf1: Clear CAN ring buffer.
    case 0xf1:
      if (req->param1 == 0xFFFFU) {
        puts("Clearing CAN Rx queue\n");
        can_clear(&can_rx_q);
      } else if (req->param1 < BUS_CNT) {
        puts("Clearing CAN Tx queue\n");
        can_clear(can_queues[req->param1]);
      } else {
        puts("Clearing CAN CAN ring buffer failed: wrong bus number\n");
      }
      break;
    // **** 0xf2: Clear UART ring buffer.
    case 0xf2:
      {
        uart_ring * rb = get_ring_by_number(req->param1);
        if (rb != NULL) {
          puts("Clearing UART queue.\n");
          clear_uart_buff(rb);
        }
        break;
      }
    // **** 0xf3: Heartbeat. Resets heartbeat counter.
    case 0xf3:
      {
        heartbeat_counter = 0U;
        heartbeat_lost = false;
        heartbeat_disabled = false;
        heartbeat_engaged = (req->param1 == 1U);
        break;
      }
    // **** 0xf4: k-line/l-line 5 baud initialization
    case 0xf4:
      if(current_board->has_lin) {
        bool k = (req->param1 == 0U) || (req->param1 == 2U);
        bool l = (req->param1 == 1U) || (req->param1 == 2U);
        uint8_t five_baud_addr = (req->param2 & 0xFFU);
        if (bitbang_five_baud_addr(k, l, five_baud_addr)) {
          resp_len = -1; // do not clear NAK yet (wait for bit banging to finish)
        }
      }
      break;
    // **** 0xf5: set clock source mode
    case 0xf5:
      current_board->set_clock_source_mode(req->param1);
      break;
    // **** 0xf6: set siren enabled
    case 0xf6:
      siren_enabled = (req->param1 != 0U);
      break;
    // **** 0xf7: set green led enabled
    case 0xf7:
      green_led_enabled = (req->param1 != 0U);
      break;
#ifdef ALLOW_DEBUG
    // **** 0xf8: disable heartbeat checks
    case 0xf8:
      heartbeat_disabled = true;
      break;
#endif
    // **** 0xde: set CAN FD data bitrate
    case 0xf9:
      if (req->param1 < CAN_CNT) {
        // TODO: add sanity check, ideally check if value is correct (from array of correct values)
        bus_config[req->param1].can_data_speed = req->param2;
        bus_config[req->param1].canfd_enabled = (req->param2 >= bus_config[req->param1].can_speed);
        bus_config[req->param1].brs_enabled = (req->param2 > bus_config[req->param1].can_speed);
        bool ret = can_init(CAN_NUM_FROM_BUS_NUM(req->param1));
        UNUSED(ret);
      }
      break;
    // **** 0xfa: check if CAN FD and BRS are enabled
    case 0xfa:
      if (req->param1 < CAN_CNT) {
        resp[0] =  bus_config[req->param1].canfd_enabled;
        resp[1] = bus_config[req->param1].brs_enabled;
        resp_len = 2;
      }
      break;
    // **** 0xfb: allow highest power saving mode (stop) to be entered
    case 0xfb:
      deepsleep_allowed = true;
      break;
    default:
      puts("NO HANDLER ");
      puth(req->request);
      puts("\n");
      break;
  }
  return resp_len;
}
