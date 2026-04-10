#include "mock_wire.h"

/* ---------------------------------------------------------------------------
 * Observable state
 * --------------------------------------------------------------------------- */
uint8_t mock_tx_buf[MOCK_WIRE_TX_BUF];
uint8_t mock_tx_len    = 0;
uint8_t mock_last_addr = 0;

bool    mock_nack_next = false;
uint8_t mock_rx_buf[MOCK_WIRE_RX_BUF];
uint8_t mock_rx_len    = 0;

uint8_t mock_ack_addrs[MOCK_WIRE_ACK_MAX];
uint8_t mock_ack_count = 0;

/* ---------------------------------------------------------------------------
 * Internal RX FIFO position (shared across read() calls in one transaction)
 * --------------------------------------------------------------------------- */
static uint8_t s_rx_pos   = 0;
static uint8_t s_rx_avail = 0;

/* ---------------------------------------------------------------------------
 * Helper functions
 * --------------------------------------------------------------------------- */
void mock_wire_reset(void)
{
    memset(mock_tx_buf,   0, sizeof(mock_tx_buf));
    mock_tx_len    = 0;
    mock_last_addr = 0;
    mock_nack_next = false;
    memset(mock_rx_buf,   0, sizeof(mock_rx_buf));
    mock_rx_len    = 0;
    memset(mock_ack_addrs, 0, sizeof(mock_ack_addrs));
    mock_ack_count = 0;
    s_rx_pos       = 0;
    s_rx_avail     = 0;
}

void mock_wire_preload_rx(const uint8_t *data, uint8_t len)
{
    uint8_t n = (len < MOCK_WIRE_RX_BUF) ? len : MOCK_WIRE_RX_BUF;
    memcpy(mock_rx_buf, data, n);
    mock_rx_len = n;
}

/* ---------------------------------------------------------------------------
 * TwoWire singleton
 * --------------------------------------------------------------------------- */
TwoWire Wire;

/* ---------------------------------------------------------------------------
 * TwoWire method implementations
 * --------------------------------------------------------------------------- */
bool TwoWire::begin(int sda, int scl, uint32_t freq)
{
    (void)sda; (void)scl; (void)freq;
    return true;
}

void TwoWire::beginTransmission(uint8_t addr)
{
    mock_last_addr = addr;
    mock_tx_len    = 0;
}

size_t TwoWire::write(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len && mock_tx_len < MOCK_WIRE_TX_BUF; i++) {
        mock_tx_buf[mock_tx_len++] = data[i];
    }
    return len;
}

size_t TwoWire::write(uint8_t byte)
{
    if (mock_tx_len < MOCK_WIRE_TX_BUF) {
        mock_tx_buf[mock_tx_len++] = byte;
    }
    return 1;
}

uint8_t TwoWire::endTransmission(bool stop)
{
    (void)stop;

    if (mock_nack_next) {
        mock_nack_next = false;
        return 2; /* NACK on address */
    }

    /*
     * When the ACK list is non-empty (used for scan tests), only listed
     * addresses ACK. An empty list means every address ACKs (default).
     */
    if (mock_ack_count > 0) {
        for (uint8_t i = 0; i < mock_ack_count; i++) {
            if (mock_ack_addrs[i] == mock_last_addr) {
                return 0; /* ACK */
            }
        }
        return 2; /* NACK — not in list */
    }

    return 0; /* default: always ACK */
}

uint8_t TwoWire::requestFrom(uint8_t addr, uint8_t count, bool stop)
{
    (void)stop;
    mock_last_addr = addr;
    s_rx_pos   = 0;
    s_rx_avail = (count < mock_rx_len) ? count : mock_rx_len;
    return s_rx_avail;
}

int TwoWire::available(void)
{
    return (int)s_rx_avail;
}

int TwoWire::read(void)
{
    if (s_rx_avail == 0) {
        return -1;
    }
    s_rx_avail--;
    return (int)mock_rx_buf[s_rx_pos++];
}
