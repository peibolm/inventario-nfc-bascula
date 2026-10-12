#include "scale_task.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "scale_uart.h"
#include "app_events.h"
#include "settings.h"

static const char *TAG = "scale_task";

#define SCALE_BAUD_RATE 9600 /* confirmado con la báscula real: 9600 8N1 */

/* Tope de espera de cada lectura de trama. No es un periodo de muestreo
 * real: si la bascula ya ha dejado tramas en el buffer, scale_uart_read_frame()
 * vuelve sin esperar, asi que la tarea va al ritmo al que transmita la
 * bascula. Solo acota cuanto se bloquea cuando no llega nada. */
#define SCALE_SAMPLE_PERIOD_MS       200

/* Tolerancia, ventana de estabilidad y timeout son ajustables en vivo desde
 * el menu de Ajustes (ver settings.h) - se leen con settings_get() en cada
 * uso, no son constantes. SCALE_STABLE_WINDOW_SAMPLES_MAX es solo el tope
 * para reservar el array de la ventana en la pila: el tamano de ventana
 * REAL en cada pesada es settings_get(SETTING_SCALE_STABLE_WINDOW_SAMPLES),
 * siempre <= este tope porque asi lo valida settings_set(). */
#define SCALE_STABLE_WINDOW_SAMPLES_MAX 20

static volatile bool s_weighing_active = false;
static volatile float s_min_weight_g = 0.0f;

/* Pesada nueva: vaciar la UART y empezar la ventana de cero. */
static volatile bool s_restart_window = false;
/* Volver a contar el timeout desde ahora, conservando la ventana. */
static volatile bool s_restart_clock = false;

void scale_request_weighing(float min_weight_g)
{
    s_min_weight_g = min_weight_g;
    /* El flush lo hace la propia tarea al ver s_restart_window, no se llama
     * aqui a scale_uart_flush(): la tarea lee la UART continuamente, asi
     * que vaciarla desde otra tarea podria pillarla a mitad de montar una
     * trama y dejar s_frame_buf a medias. */
    s_restart_window = true;
    s_restart_clock = true;
    s_weighing_active = true;
}

void scale_continue_weighing(float min_weight_g)
{
    /* Sin flush ni reinicio de ventana: sigue deslizando las muestras ya
     * acumuladas. Si cada re-armado de la vigilancia en segundo plano
     * reiniciase la ventana desde cero, el indicador de estable pasaria
     * brevemente por "inestable" en cada ciclo aunque el peso real no
     * haya cambiado nada (parpadeo rojo/verde).
     *
     * El reloj del timeout SI se reinicia: cada re-armado es una espera
     * nueva. Sin esto, como la ventana no vuelve a estar vacia nunca, el
     * reloj solo avanzaba con lecturas estables; en cuanto el operario
     * pasaba mas de SETTING_SCALE_STABILIZE_TIMEOUT_S moviendo peso (lo
     * normal contando tandas: vaciar el recipiente, echar la siguiente),
     * saltaba el timeout, app_fsm re-armaba, y la condicion seguia siendo
     * cierta en la iteracion siguiente -> un timeout por muestra, en
     * bucle, hasta que el peso volviera a asentarse. */
    s_min_weight_g = min_weight_g;
    s_restart_clock = true;
    s_weighing_active = true;
}

void scale_cancel_weighing(void)
{
    s_weighing_active = false;
}

static void scale_poll_task(void *arg)
{
    float window[SCALE_STABLE_WINDOW_SAMPLES_MAX];
    size_t window_count = 0;
    size_t window_size = SCALE_STABLE_WINDOW_SAMPLES_MAX;
    TickType_t weighing_start = xTaskGetTickCount();

    while (1) {
        /* La UART se lee SIEMPRE, haya pesada en curso o no.
         *
         * Antes la tarea dormia 200ms cuando no habia pesada activa, y eso
         * incluye el hueco entre publicar APP_EVT_SCALE_STABLE/TIMEOUT (que
         * ponen s_weighing_active a false) y el re-armado desde app_fsm. La
         * bascula transmite en continuo, asi que durante ese hueco las
         * tramas se acumulaban en el driver; al reanudar,
         * scale_uart_read_frame() las devolvia de golpe y sin esperar
         * (vuelve en cuanto hay un '\n' en el buffer), y la ventana acababa
         * mezclando pesos de instantes distintos. Con el peso quieto no se
         * nota -las tramas atrasadas valen lo mismo que la actual-, pero
         * contando tandas, donde el peso cambia mientras se echan piezas,
         * hacia que max-min no bajase nunca de la tolerancia: la bascula
         * estaba quieta y aun asi no estabilizaba nunca. */
        uint8_t frame[64];
        size_t n = scale_uart_read_frame(frame, sizeof(frame), SCALE_SAMPLE_PERIOD_MS);

        if (s_restart_window) {
            s_restart_window = false;
            scale_uart_flush();
            n = 0; /* lo que se acaba de leer es anterior al flush */
            window_count = 0;
        }

        if (window_count == 0) {
            /* El tamano de ventana se fija solo al arrancar una pesada
             * nueva, no en mitad de una: si cambiase a media ventana, las
             * muestras ya acumuladas dejarian de tener sentido. */
            int configured = (int)settings_get(SETTING_SCALE_STABLE_WINDOW_SAMPLES);
            if (configured < 1) {
                configured = 1;
            } else if (configured > SCALE_STABLE_WINDOW_SAMPLES_MAX) {
                configured = SCALE_STABLE_WINDOW_SAMPLES_MAX;
            }
            window_size = (size_t)configured;
        }

        if (s_restart_clock) {
            s_restart_clock = false;
            weighing_start = xTaskGetTickCount();
        }

        float weight_g = 0.0f;
        bool have_sample = (n > 0 && scale_protocol_parse(frame, n, &weight_g) &&
                            fabsf(weight_g) >= s_min_weight_g);
        bool is_stable = false;
        float avg = weight_g;

        if (have_sample) {
            if (window_count < window_size) {
                window[window_count++] = weight_g;
            } else {
                memmove(&window[0], &window[1], (window_size - 1) * sizeof(float));
                window[window_size - 1] = weight_g;
            }

            if (window_count == window_size) {
                float min_w = window[0], max_w = window[0], sum = 0.0f;
                for (size_t i = 0; i < window_size; i++) {
                    if (window[i] < min_w) min_w = window[i];
                    if (window[i] > max_w) max_w = window[i];
                    sum += window[i];
                }
                is_stable = (max_w - min_w) <= settings_get(SETTING_SCALE_WEIGHT_TOLERANCE_G);
                avg = sum / window_size;
            }
        }

        /* Hasta aqui se trabaja igual con pesada o sin ella: lo unico que
         * cambia es que sin pesada activa no se publica nada. Asi la
         * ventana llega siempre al dia al siguiente re-armado. */
        if (!s_weighing_active) {
            continue;
        }

        if (have_sample) {
            /* Lectura "en vivo" para dar feedback continuo en pantalla
             * (peso actual + si esta estable o aun asentandose), tanto si
             * la ventana ya esta completa/estable como si no. */
            app_event_t reading_evt = {.type = APP_EVT_SCALE_READING};
            reading_evt.data.scale_reading.weight_g = weight_g;
            reading_evt.data.scale_reading.stable = is_stable;
            app_events_post(&reading_evt);

            if (is_stable) {
                ESP_LOGI(TAG, "Peso estable: %.1f g", avg);

                /* Importante: dejar nuestro propio estado en "terminado"
                 * ANTES de publicar el evento. app_events_post() puede
                 * ceder la CPU de inmediato a app_fsm (mayor prioridad),
                 * que puede re-armar esta misma pesada (p.ej. vigilancia
                 * en WAIT_WEIGHT_USED) antes de que esta tarea retome la
                 * ejecucion; si el reseteo fuera despues del post,
                 * pisariamos ese nuevo "activo" con un "false" viejo. */
                s_weighing_active = false;

                app_event_t evt = {.type = APP_EVT_SCALE_STABLE};
                evt.data.scale_stable.weight_g = avg;
                app_events_post(&evt);
                continue;
            }
        }

        uint32_t timeout_ms = (uint32_t)(settings_get(SETTING_SCALE_STABILIZE_TIMEOUT_S) * 1000.0f);
        /* s_restart_clock pendiente = app_fsm acaba de re-armar en mitad de
         * esta iteracion; el reloj se pondra a cero al principio de la
         * siguiente, asi que no tiene sentido medir contra el viejo. */
        if (s_weighing_active && !s_restart_clock &&
            (xTaskGetTickCount() - weighing_start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGW(TAG, "Timeout esperando peso estable");
            s_weighing_active = false;

            app_event_t evt = {.type = APP_EVT_SCALE_TIMEOUT};
            app_events_post(&evt);
        }
    }
}

esp_err_t scale_task_start(void)
{
    esp_err_t ret = scale_uart_init(SCALE_BAUD_RATE, UART_DATA_8_BITS, UART_PARITY_DISABLE, UART_STOP_BITS_1);
    if (ret != ESP_OK) {
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(scale_poll_task, "scale_poll", 4096, NULL, 4, NULL);
    return (task_ret == pdPASS) ? ESP_OK : ESP_FAIL;
}
