/*
 * Tarea que lee UART1 y aplica la lógica de estabilización de peso pedida:
 * en vez de depender de un bit "estable" de la trama, espera a que las
 * últimas lecturas se mantengan dentro de una tolerancia durante una
 * ventana de tiempo. Nunca toca LVGL: solo envía eventos a la cola de la app.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Inicializa UART1 y arranca la tarea de báscula (en reposo hasta que se
 *  pida una pesada con scale_request_weighing()). */
esp_err_t scale_task_start(void);

/** Arma una nueva ventana de estabilización: la tarea empezará a acumular
 *  muestras y, al estabilizarse (o agotar el timeout), enviará
 *  APP_EVT_SCALE_STABLE / APP_EVT_SCALE_TIMEOUT.
 *
 *  @param min_weight_g Lecturas con |peso| por debajo de este valor se
 *  ignoran por completo (ni cuentan para la ventana de estabilidad ni la
 *  reinician); util para descartar ruido/tara mientras aun no se ha
 *  colocado la caja en la bascula. Pasar 0.0f para no filtrar nada. */
void scale_request_weighing(float min_weight_g);

/**
 * @brief Igual que scale_request_weighing(), pero sin vaciar el buffer de
 * UART ni reiniciar la ventana de muestras ya acumulada: la usa la propia
 * vigilancia en segundo plano (WAIT_WEIGHT_USED, REG_WAIT_TARE,
 * PARTIAL_COUNT) para volver a armarse tras cada lectura sin perder la
 * ventana ya estable (si no, el indicador de estable parpadearia en cada
 * re-armado aunque el peso real no cambie). Usar scale_request_weighing()
 * en vez de esta para arrancar una pesada realmente nueva.
 *
 * El reloj del timeout de estabilizacion SI vuelve a cero: cada re-armado
 * cuenta como una espera nueva.
 */
void scale_continue_weighing(float min_weight_g);

/** Aborta una pesada en curso sin emitir ningún evento. */
void scale_cancel_weighing(void);

#ifdef __cplusplus
}
#endif
