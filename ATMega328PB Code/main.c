#define F_CPU 16000000UL

#include <avr/io.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <util/delay.h>

#include "uart.h"

#define BNO085_PACKET_MAX_PAYLOAD      48
#define BNO085_MAX_PACKET_LENGTH       512
#define BNO085_SPI_READ_TIMEOUT_MS     500U
#define BNO085_ACCEL_REPORT_ID         0x01
#define BNO085_GYRO_REPORT_ID          0x02
#define BNO085_CHANNEL_CONTROL         2
#define BNO085_CHANNEL_REPORTS         3
#define BNO085_CHANNEL_COMMAND         0
#define BNO085_SHTP_PRODUCT_ID_REQ     0xF9
#define BNO085_SHTP_PRODUCT_ID_RESP    0xF8
#define BNO085_SHTP_SET_FEATURE_CMD    0xFD
#define BNO085_SHTP_BASE_TIMESTAMP     0xFB
#define BNO085_SHTP_CMD_ADVERTISE      0x00
#define BNO085_SHTP_CMD_ADVERTISE_ALL  0x01
#define BNO085_REPORT_INTERVAL_US      200000UL
#define BNO085_STARTUP_TIMEOUT_MS      3000U
#define BNO085_PRODUCT_REQ_PERIOD_MS   250U
#define BNO085_SPI_MODE_PRIMARY         3U
#define BNO085_SPI_MODE_FALLBACK        0U
#define BNO085_ACCEL_Q_POINT           8
#define BNO085_GYRO_Q_POINT            9

#define BNO085_SPI_PORT                PORTB
#define BNO085_SPI_DDR                 DDRB
#define BNO085_SPI_PIN                 PINB
#define BNO085_PIN_CS                  PORTB2
#define BNO085_PIN_MOSI                PORTB3
#define BNO085_PIN_MISO                PORTB4
#define BNO085_PIN_SCK                 PORTB5

#define BNO085_CTRL_PORT               PORTD
#define BNO085_CTRL_DDR                DDRD
#define BNO085_CTRL_PIN                PIND
#define BNO085_PIN_INT                 PORTD2
#define BNO085_PIN_WAKE                PORTD3
#define BNO085_PIN_RST                 PORTD4

typedef struct
{
    uint16_t packet_length;
    uint8_t channel;
    uint8_t sequence_number;
} bno085_header_t;

typedef struct
{
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
    uint8_t accel_accuracy;
    uint8_t gyro_accuracy;
    uint8_t accel_valid;
    uint8_t gyro_valid;
} imu_sample_t;

static uint8_t g_bno085_tx_sequence[6];
static uint8_t g_bno085_control_channel = BNO085_CHANNEL_CONTROL;

static void bno085_chip_select_low(void)
{
    BNO085_SPI_PORT &= (uint8_t)~(1 << BNO085_PIN_CS);
}

static void bno085_chip_select_high(void)
{
    BNO085_SPI_PORT |= (1 << BNO085_PIN_CS);
}

static void bno085_wake_high(void)
{
    BNO085_CTRL_PORT |= (1 << BNO085_PIN_WAKE);
}

static uint8_t bno085_int_asserted(void)
{
    return ((BNO085_CTRL_PIN & (1 << BNO085_PIN_INT)) == 0U);
}

static void spi_set_mode(uint8_t mode)
{
    uint8_t spcr;

    spcr = (1 << SPE) | (1 << MSTR) | (1 << SPR0);
    if ((mode & 0x02U) != 0U)
    {
        spcr |= (1 << CPOL);
    }
    if ((mode & 0x01U) != 0U)
    {
        spcr |= (1 << CPHA);
    }

    SPCR0 = spcr;
    SPSR0 = 0x00;
}

static void spi_init(void)
{
    PRR0 &= (uint8_t)~(1 << PRSPI0);

    BNO085_SPI_DDR |= (1 << BNO085_PIN_CS) | (1 << BNO085_PIN_MOSI) | (1 << BNO085_PIN_SCK);
    BNO085_SPI_DDR &= (uint8_t)~(1 << BNO085_PIN_MISO);
    BNO085_SPI_PORT &= (uint8_t)~(1 << BNO085_PIN_MISO);
    bno085_chip_select_high();

    BNO085_CTRL_DDR |= (1 << BNO085_PIN_WAKE) | (1 << BNO085_PIN_RST);
    BNO085_CTRL_DDR &= (uint8_t)~(1 << BNO085_PIN_INT);
    BNO085_CTRL_PORT |= (1 << BNO085_PIN_INT);
    BNO085_CTRL_PORT |= (1 << BNO085_PIN_WAKE) | (1 << BNO085_PIN_RST);

    spi_set_mode(BNO085_SPI_MODE_PRIMARY);
}

static uint8_t spi_transfer(uint8_t value)
{
    SPDR0 = value;
    while ((SPSR0 & (1 << SPIF)) == 0U)
    {
    }
    return SPDR0;
}

static void bno085_reset_pulse(void)
{
    bno085_wake_high();
    BNO085_CTRL_PORT |= (1 << BNO085_PIN_RST);
    _delay_ms(10);
    BNO085_CTRL_PORT &= (uint8_t)~(1 << BNO085_PIN_RST);
    _delay_ms(10);
    BNO085_CTRL_PORT |= (1 << BNO085_PIN_RST);
    _delay_ms(10);
}

static uint8_t bno085_wait_for_interrupt(uint16_t timeout_ms)
{
    uint16_t elapsed;

    elapsed = 0;
    while (elapsed < timeout_ms)
    {
        if (bno085_int_asserted())
        {
            return 1;
        }

        _delay_ms(1);
        elapsed++;
    }

    return 0;
}

static uint8_t bno085_discard_bytes(uint16_t length)
{
    while (length > 0U)
    {
        (void)spi_transfer(0x00);
        length--;
    }

    return 1;
}

static uint8_t bno085_send_packet(uint8_t channel, const uint8_t *payload, uint8_t payload_length)
{
    uint8_t tx_buffer[4 + 17];
    uint16_t packet_length;
    uint8_t index;

    packet_length = (uint16_t)payload_length + 4U;

    tx_buffer[0] = (uint8_t)(packet_length & 0xFFU);
    tx_buffer[1] = (uint8_t)((packet_length >> 8) & 0x7FU);
    tx_buffer[2] = channel;
    tx_buffer[3] = g_bno085_tx_sequence[channel]++;

    for (index = 0; index < payload_length; index++)
    {
        tx_buffer[4U + index] = payload[index];
    }

    bno085_chip_select_low();
    for (index = 0; index < (uint8_t)(payload_length + 4U); index++)
    {
        (void)spi_transfer(tx_buffer[index]);
    }
    bno085_chip_select_high();

    return 1;
}

static uint8_t bno085_receive_packet(uint8_t *payload, uint8_t payload_capacity, bno085_header_t *header)
{
    uint8_t rx_header_probe[4];
    uint8_t rx_header[4];
    uint16_t first_packet_length;
    uint16_t payload_length;
    uint16_t index;

    if (!bno085_wait_for_interrupt(BNO085_SPI_READ_TIMEOUT_MS))
    {
        return 0;
    }

    bno085_chip_select_low();
    for (index = 0; index < 4U; index++)
    {
        rx_header_probe[index] = spi_transfer(0x00);
    }
    bno085_chip_select_high();

    first_packet_length = (uint16_t)((uint16_t)rx_header_probe[1] << 8) | rx_header_probe[0];
    first_packet_length &= 0x7FFFU;

    if ((first_packet_length < 4U) || (first_packet_length > BNO085_MAX_PACKET_LENGTH))
    {
        return 0;
    }

    if (!bno085_wait_for_interrupt(BNO085_SPI_READ_TIMEOUT_MS))
    {
        return 0;
    }

    bno085_chip_select_low();
    for (index = 0; index < 4U; index++)
    {
        rx_header[index] = spi_transfer(0x00);
    }

    header->packet_length = (uint16_t)((uint16_t)rx_header[1] << 8) | rx_header[0];
    header->packet_length &= 0x7FFFU;
    header->channel = rx_header[2];
    header->sequence_number = rx_header[3];

    if ((header->packet_length < 4U) || (header->packet_length > BNO085_MAX_PACKET_LENGTH))
    {
        bno085_chip_select_high();
        return 0;
    }

    payload_length = (uint16_t)(header->packet_length - 4U);
    if (payload_length > payload_capacity)
    {
        (void)bno085_discard_bytes(payload_length);
        bno085_chip_select_high();
        return 0;
    }

    for (index = 0; index < payload_length; index++)
    {
        payload[index] = spi_transfer(0x00);
    }

    bno085_chip_select_high();
    return (payload_length > 0U);
}

static uint32_t bno085_make_u32(const uint8_t *data)
{
    return ((uint32_t)data[3] << 24)
         | ((uint32_t)data[2] << 16)
         | ((uint32_t)data[1] << 8)
         | (uint32_t)data[0];
}

static int16_t bno085_make_s16(const uint8_t *data)
{
    return (int16_t)(((uint16_t)data[1] << 8) | data[0]);
}

static void print_signed_thousandths(int32_t value_thousandths)
{
    uint32_t magnitude;

    if (value_thousandths < 0)
    {
        putchar('-');
        magnitude = (uint32_t)(-value_thousandths);
    }
    else
    {
        magnitude = (uint32_t)value_thousandths;
    }

    printf("%lu.%03lu", (unsigned long)(magnitude / 1000UL), (unsigned long)(magnitude % 1000UL));
}

static int32_t q_to_thousandths(int16_t raw_value, uint8_t q_point)
{
    int32_t scaled;
    int32_t divisor;

    scaled = (int32_t)raw_value * 1000L;
    divisor = (int32_t)1 << q_point;

    return scaled / divisor;
}

static void print_sensor_line(const imu_sample_t *sample)
{
    printf("ACC[m/s^2]=(");
    print_signed_thousandths(q_to_thousandths(sample->accel_x, BNO085_ACCEL_Q_POINT));
    printf(", ");
    print_signed_thousandths(q_to_thousandths(sample->accel_y, BNO085_ACCEL_Q_POINT));
    printf(", ");
    print_signed_thousandths(q_to_thousandths(sample->accel_z, BNO085_ACCEL_Q_POINT));

    printf(")  GYRO[rad/s]=(");
    print_signed_thousandths(q_to_thousandths(sample->gyro_x, BNO085_GYRO_Q_POINT));
    printf(", ");
    print_signed_thousandths(q_to_thousandths(sample->gyro_y, BNO085_GYRO_Q_POINT));
    printf(", ");
    print_signed_thousandths(q_to_thousandths(sample->gyro_z, BNO085_GYRO_Q_POINT));
    printf(")  acc=%u gyro=%u\r\n", sample->accel_accuracy, sample->gyro_accuracy);
}

static uint8_t bno085_request_product_id(void)
{
    uint8_t payload[2];

    payload[0] = BNO085_SHTP_PRODUCT_ID_REQ;
    payload[1] = 0x00;

    return bno085_send_packet(g_bno085_control_channel, payload, sizeof(payload));
}

static uint8_t bno085_request_product_id_on_channel(uint8_t channel)
{
    uint8_t saved_control_channel;
    uint8_t ok;

    saved_control_channel = g_bno085_control_channel;
    g_bno085_control_channel = channel;
    ok = bno085_request_product_id();
    g_bno085_control_channel = saved_control_channel;

    return ok;
}

static uint8_t bno085_request_advertise_all(void)
{
    uint8_t payload[2];

    payload[0] = BNO085_SHTP_CMD_ADVERTISE;
    payload[1] = BNO085_SHTP_CMD_ADVERTISE_ALL;

    return bno085_send_packet(BNO085_CHANNEL_COMMAND, payload, sizeof(payload));
}

static void bno085_probe_product_id_channels(void)
{
    static const uint8_t candidate_channels[] = {2U, 1U, 3U, 4U, 5U};
    uint8_t i;

    (void)bno085_request_product_id_on_channel(g_bno085_control_channel);
    for (i = 0; i < (uint8_t)sizeof(candidate_channels); i++)
    {
        if (candidate_channels[i] != g_bno085_control_channel)
        {
            (void)bno085_request_product_id_on_channel(candidate_channels[i]);
        }
    }
}

static uint8_t bno085_set_feature(uint8_t report_id, uint32_t report_interval_us)
{
    uint8_t payload[17];

    memset(payload, 0, sizeof(payload));
    payload[0] = BNO085_SHTP_SET_FEATURE_CMD;
    payload[1] = report_id;
    payload[5] = (uint8_t)(report_interval_us & 0xFFU);
    payload[6] = (uint8_t)((report_interval_us >> 8) & 0xFFU);
    payload[7] = (uint8_t)((report_interval_us >> 16) & 0xFFU);
    payload[8] = (uint8_t)((report_interval_us >> 24) & 0xFFU);

    return bno085_send_packet(g_bno085_control_channel, payload, sizeof(payload));
}

static void bno085_handle_control_packet(const uint8_t *payload, uint16_t payload_length)
{
    uint32_t part_number;
    uint32_t build_number;
    uint16_t patch_version;

    if ((payload_length >= 14U) && (payload[0] == BNO085_SHTP_PRODUCT_ID_RESP))
    {
        part_number = bno085_make_u32(&payload[4]);
        build_number = bno085_make_u32(&payload[8]);
        patch_version = (uint16_t)(((uint16_t)payload[13] << 8) | payload[12]);

        printf("BNO085 Product ID: sw=%u.%u.%u part=%lu build=%lu\r\n",
               payload[2],
               payload[3],
               patch_version,
               (unsigned long)part_number,
               (unsigned long)build_number);
    }
}

static void bno085_parse_sensor_packet(const uint8_t *payload, uint16_t payload_length, imu_sample_t *sample)
{
    uint16_t offset;
    uint8_t report_id;

    if ((payload_length < 16U) || (payload[0] != BNO085_SHTP_BASE_TIMESTAMP))
    {
        return;
    }

    offset = 5U;
    while ((offset + 10U) < payload_length)
    {
        report_id = payload[offset];

        if (report_id == BNO085_ACCEL_REPORT_ID)
        {
            sample->accel_x = bno085_make_s16(&payload[offset + 5U]);
            sample->accel_y = bno085_make_s16(&payload[offset + 7U]);
            sample->accel_z = bno085_make_s16(&payload[offset + 9U]);
            sample->accel_accuracy = payload[offset + 2U] & 0x03U;
            sample->accel_valid = 1U;
            offset = (uint16_t)(offset + 11U);
        }
        else if (report_id == BNO085_GYRO_REPORT_ID)
        {
            sample->gyro_x = bno085_make_s16(&payload[offset + 5U]);
            sample->gyro_y = bno085_make_s16(&payload[offset + 7U]);
            sample->gyro_z = bno085_make_s16(&payload[offset + 9U]);
            sample->gyro_accuracy = payload[offset + 2U] & 0x03U;
            sample->gyro_valid = 1U;
            offset = (uint16_t)(offset + 11U);
        }
        else
        {
            break;
        }
    }
}

static uint8_t bno085_wait_for_startup(uint16_t timeout_ms)
{
    uint8_t payload[BNO085_PACKET_MAX_PAYLOAD];
    bno085_header_t header;
    uint16_t payload_length;
    uint16_t elapsed;
    uint16_t product_req_elapsed;
    uint8_t packet_seen;
    uint8_t suspicious_packet_count;
    uint8_t suspicious_warning_printed;

    elapsed = 0;
    product_req_elapsed = BNO085_PRODUCT_REQ_PERIOD_MS;
    suspicious_packet_count = 0U;
    suspicious_warning_printed = 0U;
    while (elapsed < timeout_ms)
    {
        if (product_req_elapsed >= BNO085_PRODUCT_REQ_PERIOD_MS)
        {
            (void)bno085_request_advertise_all();
            bno085_probe_product_id_channels();
            product_req_elapsed = 0;
        }

        packet_seen = 0U;
        while (bno085_receive_packet(payload, sizeof(payload), &header))
        {
            packet_seen = 1U;
            payload_length = (uint16_t)(header.packet_length - 4U);

            if ((header.channel == 0U) && (payload_length == 16U) && (header.sequence_number == 255U))
            {
                if (suspicious_packet_count < 255U)
                {
                    suspicious_packet_count++;
                }

                if ((suspicious_packet_count >= 4U) && !suspicious_warning_printed)
                {
                    printf("Repeated ch0/len16/seq255 packets. Check SPI mode/wiring (MISO, CS, CPOL/CPHA).\r\n");
                    suspicious_warning_printed = 1U;
                }
            }

            if (header.channel == BNO085_CHANNEL_CONTROL)
            {
                bno085_handle_control_packet(payload, payload_length);
                if ((payload_length >= 14U) && (payload[0] == BNO085_SHTP_PRODUCT_ID_RESP))
                {
                    g_bno085_control_channel = header.channel;
                    return 1;
                }

                if (payload_length > 0U)
                {
                    printf("Startup CTRL packet: id=0x%02X len=%u seq=%u\r\n",
                           payload[0],
                           payload_length,
                           header.sequence_number);
                }
            }
            else
            {
                if (payload_length > 0U)
                {
                    if ((payload[0] >= 0xF0U) && (payload[0] <= 0xFEU))
                    {
                        g_bno085_control_channel = header.channel;
                    }

                    printf("Startup packet: ch=%u id=0x%02X len=%u seq=%u\r\n",
                           header.channel,
                           payload[0],
                           payload_length,
                           header.sequence_number);
                }
                else
                {
                    printf("Startup packet: ch=%u len=%u seq=%u\r\n",
                           header.channel,
                           payload_length,
                           header.sequence_number);
                }
            }
        }

        if (packet_seen)
        {
            continue;
        }

        _delay_ms(10);
        elapsed = (uint16_t)(elapsed + 10U);
        product_req_elapsed = (uint16_t)(product_req_elapsed + 10U);
    }

    return 0;
}

static void bno085_flush_packets(uint16_t flush_time_ms)
{
    uint8_t payload[BNO085_PACKET_MAX_PAYLOAD];
    bno085_header_t header;
    uint16_t elapsed;

    elapsed = 0;
    while (elapsed < flush_time_ms)
    {
        while (bno085_receive_packet(payload, sizeof(payload), &header))
        {
        }

        _delay_ms(10);
        elapsed = (uint16_t)(elapsed + 10U);
    }
}

static uint8_t bno085_initialize_with_mode(uint8_t spi_mode)
{
    spi_set_mode(spi_mode);
    memset(g_bno085_tx_sequence, 0, sizeof(g_bno085_tx_sequence));
    g_bno085_control_channel = BNO085_CHANNEL_CONTROL;

    printf("\r\nStarting BNO085 over SPI...\r\n");
    printf("SPI mode: %u\r\n", (unsigned int)spi_mode);
    printf("Pins: CS=PB2 MOSI=PB3 MISO=PB4 SCK=PB5 INT=PD2 WAKE=PD3 RST=PD4\r\n");

    bno085_reset_pulse();
    if (!bno085_wait_for_interrupt(1000))
    {
        printf("No interrupt after reset. Check INT, WAKE, RST, and SPI mode straps.\r\n");
        return 0;
    }

    bno085_flush_packets(200);

    if (!bno085_wait_for_startup(BNO085_STARTUP_TIMEOUT_MS))
    {
        printf("No product ID response from BNO085 over SPI in mode %u.\r\n",
               (unsigned int)spi_mode);
        return 0;
    }

    if (!bno085_set_feature(BNO085_ACCEL_REPORT_ID, BNO085_REPORT_INTERVAL_US))
    {
        printf("Failed to enable accelerometer reports.\r\n");
        return 0;
    }

    if (!bno085_set_feature(BNO085_GYRO_REPORT_ID, BNO085_REPORT_INTERVAL_US))
    {
        printf("Failed to enable gyro reports.\r\n");
        return 0;
    }

    bno085_flush_packets(100);
    printf("Streaming accelerometer and gyro data every %lu ms.\r\n",
           (unsigned long)(BNO085_REPORT_INTERVAL_US / 1000UL));
    return 1;
}

static uint8_t bno085_initialize(void)
{
    if (bno085_initialize_with_mode(BNO085_SPI_MODE_PRIMARY))
    {
        return 1;
    }

    if (BNO085_SPI_MODE_FALLBACK != BNO085_SPI_MODE_PRIMARY)
    {
        printf("Retrying initialization with SPI mode %u.\r\n",
               (unsigned int)BNO085_SPI_MODE_FALLBACK);
        if (bno085_initialize_with_mode(BNO085_SPI_MODE_FALLBACK))
        {
            return 1;
        }
    }

    return 0;
}

void Initialize(void)
{
    uart_init();
    spi_init();
}

int main(void)
{
    uint8_t payload[BNO085_PACKET_MAX_PAYLOAD];
    bno085_header_t header;
    imu_sample_t sample;
    uint8_t imu_ready;

    memset(&sample, 0, sizeof(sample));
    Initialize();

    printf("\r\nATmega328PB + BNO085 demo\r\n");
    printf("UART: 9600 baud, 8 data bits, 2 stop bits\r\n");

    imu_ready = 0U;

    while (1)
    {
        if (!imu_ready)
        {
            imu_ready = bno085_initialize();
            if (!imu_ready)
            {
                printf("Retrying IMU initialization in 1 second...\r\n");
                _delay_ms(1000);
            }
            continue;
        }

        if (!bno085_receive_packet(payload, sizeof(payload), &header))
        {
            _delay_ms(10);
            continue;
        }

        if (header.channel == BNO085_CHANNEL_REPORTS)
        {
            bno085_parse_sensor_packet(payload, (uint16_t)(header.packet_length - 4U), &sample);

            if (sample.accel_valid && sample.gyro_valid)
            {
                print_sensor_line(&sample);
                sample.accel_valid = 0U;
                sample.gyro_valid = 0U;
            }
        }
        else if (header.channel == BNO085_CHANNEL_CONTROL)
        {
            bno085_handle_control_packet(payload, (uint16_t)(header.packet_length - 4U));
        }
    }
}
