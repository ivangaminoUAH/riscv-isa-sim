#include <sys/time.h>
#include "devices.h"
#include "processor.h"
#include "term.h"

#define UART_QUEUE_SIZE         64

#define UART_RX                 0 /* In:  Receive buffer */
#define UART_TX                 0 /* Out: Transmit buffer */

#define UART_IER                1 /* Out: Interrupt Enable Register */
#define UART_IER_MSI            0x08 /* Enable Modem status interrupt */
#define UART_IER_RLSI           0x04 /* Enable receiver line status interrupt */
#define UART_IER_THRI           0x02 /* Enable Transmitter holding register int. */
#define UART_IER_RDI            0x01 /* Enable receiver data interrupt */

#define UART_IIR                2 /* In:  Interrupt ID Register */
#define UART_IIR_NO_INT         0x01 /* No interrupts pending */
#define UART_IIR_ID             0x0e /* Mask for the interrupt ID */
#define UART_IIR_MSI            0x00 /* Modem status interrupt */
#define UART_IIR_THRI           0x02 /* Transmitter holding register empty */
#define UART_IIR_RDI            0x04 /* Receiver data interrupt */
#define UART_IIR_RLSI           0x06 /* Receiver line status interrupt */

#define UART_IIR_TYPE_BITS      0xc0

#define UART_FCR                2 /* Out: FIFO Control Register */
#define UART_FCR_ENABLE_FIFO    0x01 /* Enable the FIFO */
#define UART_FCR_CLEAR_RCVR     0x02 /* Clear the RCVR FIFO */
#define UART_FCR_CLEAR_XMIT     0x04 /* Clear the XMIT FIFO */
#define UART_FCR_DMA_SELECT     0x08 /* For DMA applications */

#define UART_LCR                3 /* Out: Line Control Register */
#define UART_LCR_DLAB           0x80 /* Divisor latch access bit */
#define UART_LCR_SBC            0x40 /* Set break control */
#define UART_LCR_SPAR           0x20 /* Stick parity (?) */
#define UART_LCR_EPAR           0x10 /* Even parity select */
#define UART_LCR_PARITY         0x08 /* Parity Enable */
#define UART_LCR_STOP           0x04 /* Stop bits: 0=1 bit, 1=2 bits */

#define UART_MCR                4 /* Out: Modem Control Register */
#define UART_MCR_LOOP           0x10 /* Enable loopback test mode */
#define UART_MCR_OUT2           0x08 /* Out2 complement */
#define UART_MCR_OUT1           0x04 /* Out1 complement */
#define UART_MCR_RTS            0x02 /* RTS complement */
#define UART_MCR_DTR            0x01 /* DTR complement */

#define UART_LSR                5 /* In:  Line Status Register */
#define UART_LSR_FIFOE          0x80 /* Fifo error */
#define UART_LSR_TEMT           0x40 /* Transmitter empty */
#define UART_LSR_THRE           0x20 /* Transmit-hold-register empty */
#define UART_LSR_BI             0x10 /* Break interrupt indicator */
#define UART_LSR_FE             0x08 /* Frame error indicator */
#define UART_LSR_PE             0x04 /* Parity error indicator */
#define UART_LSR_OE             0x02 /* Overrun error indicator */
#define UART_LSR_DR             0x01 /* Receiver data ready */
#define UART_LSR_BRK_ERROR_BITS 0x1E /* BI, FE, PE, OE bits */

#define UART_MSR                6 /* In:  Modem Status Register */
#define UART_MSR_DCD            0x80 /* Data Carrier Detect */
#define UART_MSR_RI             0x40 /* Ring Indicator */
#define UART_MSR_DSR            0x20 /* Data Set Ready */
#define UART_MSR_CTS            0x10 /* Clear to Send */
#define UART_MSR_DDCD           0x08 /* Delta DCD */
#define UART_MSR_TERI           0x04 /* Trailing edge ring indicator */
#define UART_MSR_DDSR           0x02 /* Delta DSR */
#define UART_MSR_DCTS           0x01 /* Delta CTS */
#define UART_MSR_ANY_DELTA      0x0F /* Any of the delta bits! */

#define UART_SCR                7 /* I/O: Scratch Register */

ns16550_t::ns16550_t(class bus_t *bus, abstract_interrupt_controller_t *intctrl,
                     uint32_t interrupt_id, uint32_t reg_shift, uint32_t reg_io_width)
  : bus(bus), intctrl(intctrl), interrupt_id(interrupt_id), reg_shift(reg_shift), reg_io_width(reg_io_width)
{
  ier = 0;
  iir = UART_IIR_NO_INT;
  fcr = 1;
  lcr = 0;
  lsr = UART_LSR_TEMT | UART_LSR_THRE;
  msr = UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS;
  dll = 0x21;
  mcr = UART_MCR_OUT2;
  scr = 0;
}

void ns16550_t::update_interrupt(void)
{
  uint8_t interrupts = 0;

  /* Handle clear rx */
  if (fcr & UART_FCR_CLEAR_RCVR) {
    fcr &= ~UART_FCR_CLEAR_RCVR;
    while (!rx_queue.empty()) {
      rx_queue.pop();
    }
    lsr &= ~UART_LSR_DR;
  }

  /* Handle clear tx */
  if (fcr & UART_FCR_CLEAR_XMIT) {
    fcr &= ~UART_FCR_CLEAR_XMIT;
    lsr |= UART_LSR_TEMT | UART_LSR_THRE;
  }

  /* Data ready and rcv interrupt enabled ? */
  if ((ier & UART_IER_RDI) && (lsr & UART_LSR_DR)) {
    interrupts |= UART_IIR_RDI;
  }

  /* Transmitter empty and interrupt enabled ? */
  if ((ier & UART_IER_THRI) && (lsr & UART_LSR_TEMT)) {
    interrupts |= UART_IIR_THRI;
  }

  /* Now update the interrup line, if necessary */
  if (!interrupts) {
    iir = UART_IIR_NO_INT;
    intctrl->set_interrupt_level(interrupt_id, 0);
  } else {
    iir = interrupts;
    intctrl->set_interrupt_level(interrupt_id, 1);
  }

  /*
   * If the OS disabled the tx interrupt, we know that there is nothing
   * more to transmit.
   */
  if (!(ier & UART_IER_THRI)) {
    lsr |= UART_LSR_TEMT | UART_LSR_THRE;
  }
}

uint8_t ns16550_t::rx_byte(void)
{
  if (rx_queue.empty()) {
    lsr &= ~UART_LSR_DR;
    return 0;
  }

  /* Break issued ? */
  if (lsr & UART_LSR_BI) {
    lsr &= ~UART_LSR_BI;
    return 0;
  }

  uint8_t ret = rx_queue.front();
  rx_queue.pop();
  if (rx_queue.empty()) {
    lsr &= ~UART_LSR_DR;
  }

  return ret;
}

void ns16550_t::tx_byte(uint8_t val)
{
  lsr |= UART_LSR_TEMT | UART_LSR_THRE;

  std::pair <reg_t, abstract_device_t*> UART_plugin = bus->find_device(EXT_IO_BASE);
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << " abstract_device_t: " << std::to_string(UART_plugin.second) << std::endl;
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << std::endl;
  if (UART_plugin.first == EXT_IO_BASE){
    uint8_t bytes = val;

    if(bus->store(EXT_IO_BASE, 1, (const uint8_t*)&bytes)){
      //std::cout << "Transmited TX byte through ttyS3" << std::endl;
    }
  }
  else{
    canonical_terminal_t::write(val);
  }
  
}

bool ns16550_t::load(reg_t addr, size_t len, uint8_t* bytes)
{
  uint8_t val;
  bool ret = true, update = false;

  if (reg_io_width != len) {
    return false;
  }
  addr >>= reg_shift;
  addr &= 7;

  switch (addr) {
  case UART_RX:
    if (lcr & UART_LCR_DLAB) {
      val = dll;
    } else {
      val = rx_byte();
    }
    update = true;
    break;
  case UART_IER:
    if (lcr & UART_LCR_DLAB) {
      val = dlm;
    } else {
      val = ier;
    }
    break;
  case UART_IIR:
    val = iir | UART_IIR_TYPE_BITS;
    break;
  case UART_LCR:
    val = lcr;
    break;
  case UART_MCR:
    val = mcr;
    break;
  case UART_LSR:
    val = lsr;
    break;
  case UART_MSR:
    val = msr;
    break;
  case UART_SCR:
    val = scr;
    break;
  default:
    ret = false;
    break;
  };

  if (ret) {
    bytes[0] = val;
  }
  if (update) {
    update_interrupt();
  }
  //std::cerr << "LOAD: " << addr << ", " << std::to_string(val) << ", " << ret << std::endl;
  return ret;
}

bool ns16550_t::store(reg_t addr, size_t len, const uint8_t* bytes)
{
  uint8_t val;
  bool ret = true, update = false;

  if (reg_io_width != len) {
    return false;
  }
  addr >>= reg_shift;
  addr &= 7;
  val = bytes[0];

  switch (addr) {
  case UART_TX:
    update = true;

    if (lcr & UART_LCR_DLAB) {
      dll = val;
      break;
    }

    /* Loopback mode */
    if (mcr & UART_MCR_LOOP) {
      if (rx_queue.size() < UART_QUEUE_SIZE) {
        rx_queue.push(val);
        lsr |= UART_LSR_DR;
      }
      break;
    }

    tx_byte(val);
    break;
  case UART_IER:
    if (!(lcr & UART_LCR_DLAB)) {
      ier = val & 0x0f;
    } else {
      dlm = val;
    }
    update = true;
    break;
  case UART_FCR:
    fcr = val;
    update = true;
    break;
  case UART_LCR:
    lcr = val;
    update = true;
    break;
  case UART_MCR:
    mcr = val;
    update = true;
    break;
  case UART_LSR:
    /* Factory test */
    break;
  case UART_MSR:
    /* Not used */
    break;
  case UART_SCR:
    scr = val;
    break;
  default:
    ret = false;
    break;
  };

  if (update) {
    update_interrupt();
  }
  //std::cerr << "STORE: " << addr << ", " << std::to_string(val) << ", " << ret << std::endl;
  return ret;
}

void ns16550_t::tick(void)
{
  if (!(fcr & UART_FCR_ENABLE_FIFO) ||
      (mcr & UART_MCR_LOOP) ||
      (UART_QUEUE_SIZE <= rx_queue.size())) {
    return;
  }

  int rc;

  std::pair <reg_t, abstract_device_t*> UART_plugin = bus->find_device(EXT_IO_BASE);
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << " abstract_device_t: " << std::to_string(UART_plugin.second) << std::endl;
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << std::endl;
/*
  UART_plugin = bus->find_device(0x2000);
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << " abstract_device_t: " << std::to_string(UART_plugin.second) << std::endl;
  std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << std::endl;

  UART_plugin = bus->find_device(0x87FFFE00);
  //std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << " abstract_device_t: " << std::to_string(UART_plugin.second) << std::endl;
  std::cout << "reg_t: "<< std::to_string(UART_plugin.first) << std::endl;
*/

  if (UART_plugin.first == EXT_IO_BASE){
    uint8_t bytes;
    if(bus->load(EXT_IO_BASE,1,(uint8_t*)&bytes)){
      rc = bytes;
      //std::cout << "Received RX byte through ttyS3" << std::endl;
    }
    else{
      rc = -1;
    }
  }
  else{
    rc = canonical_terminal_t::read();
  }

  if (rc < 0) {
    return;
  }

  rx_queue.push((uint8_t)rc);
  lsr |= UART_LSR_DR;
  update_interrupt();
}
