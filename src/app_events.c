#include "app_events.h"

#include "esp_log.h"

static const char *TAG = "app_events";

/* La bascula puede publicar lecturas a rafagas: scale_uart_read_frame()
 * vuelve sin esperar si ya hay tramas en el buffer, y app_fsm repinta LVGL
 * en cada APP_EVT_SCALE_READING, asi que drena mas despacio de lo que la
 * bascula produce. Con la cola de 8 que habia, una rafaga la llenaba y se
 * descartaba justo lo que no se puede perder: el APP_EVT_SCALE_STABLE que
 * registra la tanda o da por vaciado el recipiente (en la pesada por partes
 * no hay alternativa manual a ese evento, a diferencia del "Confirmar" de
 * la pesada normal). */
#define APP_EVENT_QUEUE_LEN 32

/* Las lecturas en vivo son solo feedback de pantalla y la siguiente llega
 * en milisegundos, asi que ceden el paso: si ya hay esta cantidad de
 * eventos esperando, la lectura se descarta en vez de encolarse. Eso deja
 * el resto de la cola libre para los eventos que si importan (estable,
 * timeout, tag NFC, botones) y ademas evita que el peso mostrado se quede
 * atras arrastrando una cola de lecturas viejas.
 *
 * No se fusionan (sustituir el valor de una lectura ya encolada por uno
 * mas nuevo) porque eso reordenaria: una lectura posterior a un
 * APP_EVT_SCALE_STABLE se entregaria antes que el, y app_fsm usa
 * precisamente ese orden para invalidar la ultima pesada estable cuando la
 * bascula se mueve (ver partial_live_update). */
#define APP_EVENT_READING_MAX_BACKLOG 4

QueueHandle_t g_app_event_queue = NULL;

void app_events_init(void)
{
    if (g_app_event_queue != NULL) {
        return;
    }
    g_app_event_queue = xQueueCreate(APP_EVENT_QUEUE_LEN, sizeof(app_event_t));
}

void app_events_post(const app_event_t *event)
{
    if (g_app_event_queue == NULL) {
        return;
    }

    if (event->type == APP_EVT_SCALE_READING) {
        UBaseType_t pending = APP_EVENT_QUEUE_LEN - uxQueueSpacesAvailable(g_app_event_queue);
        if (pending >= APP_EVENT_READING_MAX_BACKLOG) {
            /* Sin log: que app_fsm vaya por detras de una rafaga de la
             * bascula es lo normal, y avisarlo solo taparia el aviso de
             * abajo, que si es un problema. */
            return;
        }
    }

    if (xQueueSend(g_app_event_queue, event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de eventos llena, evento %d descartado", (int)event->type);
    }
}
