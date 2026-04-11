#include "mock_uart.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Shared event log
 * --------------------------------------------------------------------------- */
mock_event_t mock_event_log[MOCK_LOG_MAX];
int          mock_event_count = 0;

void mock_log_event(mock_event_t evt)
{
    if (mock_event_count < MOCK_LOG_MAX) {
        mock_event_log[mock_event_count++] = evt;
    }
}

/* ---------------------------------------------------------------------------
 * millis() stub
 * --------------------------------------------------------------------------- */
static uint32_t s_millis_val  = 0;
static uint32_t s_millis_step = 0;

uint32_t millis(void)
{
    uint32_t v = s_millis_val;
    s_millis_val += s_millis_step;
    return v;
}

void mock_set_millis(uint32_t initial, uint32_t step)
{
    s_millis_val  = initial;
    s_millis_step = step;
}

/* ---------------------------------------------------------------------------
 * MockSerial implementation
 * --------------------------------------------------------------------------- */
MockSerial Serial1;

void MockSerial::begin(uint32_t /*baud*/, uint32_t /*config*/,
                       int8_t /*rxPin*/, int8_t /*txPin*/)
{
    /* no-op */
}

size_t MockSerial::write(const uint8_t *buf, size_t len)
{
    mock_log_event(MOCK_EVT_UART_WRITE);
    for (size_t i = 0; i < len && tx_count < BUF_SIZE; i++) {
        tx_buf[tx_count++] = buf[i];
    }
    return len;
}

void MockSerial::flush(void)
{
    /* no-op — TX is assumed instantaneous */
}

int MockSerial::available(void)
{
    return (rx_tail - rx_head);
}

int MockSerial::read(void)
{
    if (rx_head >= rx_tail) {
        return -1;
    }
    mock_log_event(MOCK_EVT_UART_READ);
    return (int)(uint8_t)rx_buf[rx_head++];
}

void MockSerial::reset(void)
{
    memset(rx_buf, 0, sizeof(rx_buf));
    rx_head  = 0;
    rx_tail  = 0;
    memset(tx_buf, 0, sizeof(tx_buf));
    tx_count = 0;
}

void MockSerial::queue_response(const uint8_t *bytes, uint8_t len)
{
    for (uint8_t i = 0; i < len && rx_tail < BUF_SIZE; i++) {
        rx_buf[rx_tail++] = bytes[i];
    }
}

int MockSerial::get_transmitted(uint8_t *buf, int max_len) const
{
    int n = (tx_count < max_len) ? tx_count : max_len;
    memcpy(buf, tx_buf, (size_t)n);
    return n;
}

int MockSerial::get_tx_count(void) const
{
    return tx_count;
}

/* ---------------------------------------------------------------------------
 * Convenience wrappers
 * --------------------------------------------------------------------------- */
void mock_uart_reset(void)
{
    Serial1.reset();
    mock_event_count = 0;
    memset(mock_event_log, 0, sizeof(mock_event_log));
    mock_set_millis(0, 0);
}

void mock_uart_queue_response(const uint8_t *bytes, uint8_t len)
{
    Serial1.queue_response(bytes, len);
}

int mock_uart_get_transmitted(uint8_t *buf, int max_len)
{
    return Serial1.get_transmitted(buf, max_len);
}

/* ---------------------------------------------------------------------------
 * delay() stub
 * --------------------------------------------------------------------------- */
void delay(uint32_t /*ms*/)
{
    /* no-op */
}
