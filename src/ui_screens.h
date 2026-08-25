/*
 * Pantallas LVGL de la app de inventario, sustituyen a lv_demo_widgets().
 * Solo app_fsm (y los propios callbacks de botón, que ya corren dentro de
 * la tarea LVGL) deben llamar a estas funciones.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Crea los widgets reutilizables sobre la pantalla activa de disp. */
void ui_screens_init(lv_disp_t *disp);

#define UI_RECENT_DESC_MAX_LEN 64
#define UI_RECENT_MAX 5

typedef struct {
    char descripcion[UI_RECENT_DESC_MAX_LEN];
    int unidades_nuevas;
    int unidades_usadas;
} ui_recent_item_t;

/**
 * @brief Pantalla de espera: "Acerque un articulo al lector", con una
 * tabla de los ultimos articulos registrados (el mas nuevo arriba).
 *
 * @param items Array de hasta UI_RECENT_MAX entradas, la mas reciente en
 * el indice 0. Puede ser NULL si count es 0.
 * @param count Numero de entradas validas en items (0..UI_RECENT_MAX).
 */
void ui_show_idle(const ui_recent_item_t *items, size_t count);

/** Pantalla de error genérica (código no encontrado, duplicado...) con
 *  botón para volver a esperar tag. */
void ui_show_error(const char *msg);

/**
 * @brief Articulo con codigo ya registrado en inventario.csv: ofrece
 * "Aceptar" (volver sin hacer nada) o "Sobrescribir" (repetir el proceso y
 * reemplazar la fila anterior de ese codigo en el inventario).
 */
void ui_show_duplicate_warning(const char *descripcion);

/**
 * @brief Al dar de alta un tag nuevo, el código tecleado ya tiene OTRO tag
 * vinculado en datos_maestros.csv (posible tag físico perdido/sustituido,
 * o error de tecleo). "Sobrescribir" re-vincula el tag que se está
 * registrando a ese código; "Cancelar" vuelve a reposo sin tocar nada.
 */
void ui_show_tag_relink_warning(const char *codigo, const char *descripcion);

/**
 * @brief Al dar de alta un tag nuevo, el código tecleado ya existe en
 * datos_maestros.csv: se muestra la descripción a toda pantalla, ANTES de
 * pedir nada más (unidades, tara...), para poder validar de un vistazo que
 * el código corresponde de verdad al material que se está contando -
 * evita arrastrar un error de tecleo o una caja mal etiquetada varios
 * pasos sin darse cuenta. "Confirmar" continúa; "Revisar código" vuelve a
 * teclearlo; "Cancelar" vuelve a reposo.
 */
void ui_show_confirm_articulo(const char *codigo, const char *descripcion);

/**
 * @brief Aviso de escritura larga en la tarjeta (reescritura completa de
 * inventario.csv al sobrescribir un código ya registrado), para que no
 * parezca que la pantalla se ha colgado.
 *
 * A propósito NO tiene ningún botón: mientras se escribe, la máquina de
 * estados está bloqueada y cualquier toque quedaría encolado para
 * ejecutarse al terminar, disparando una acción que el operario ya no
 * espera.
 *
 * Quien la use debe ceder el paso (~100 ms) antes de empezar a escribir:
 * la tarea de dibujo de LVGL tiene menos prioridad que la máquina de
 * estados, así que sin esa pausa el aviso puede no llegar a pintarse hasta
 * que la escritura ya ha terminado.
 */
void ui_show_saving(void);

/**
 * @brief Muestra la descripción del artículo y pide colocarlo en la báscula.
 *
 * @param permitir_por_partes Si true, junto a Cancelar aparece "Pesar por
 * partes" (dispara APP_EVT_UI_RETRY_PRESSED), para cajas que superan el
 * máximo de la báscula. Solo tiene sentido con artículos ya catalogados: el
 * recuento por tandas necesita el peso_unitario de datos_maestros, que en un
 * alta de material nuevo todavía no se conoce.
 */
void ui_show_description_and_wait_weight(const char *descripcion, bool permitir_por_partes);

/**
 * @brief Pide retirar los útiles nuevos. LED de estable/inestable en la
 * esquina superior izquierda; abajo, tabla con peso total/unidades
 * totales (fijos) y nuevas/usadas (en vivo, números grandes) para que se
 * vea de un vistazo que el inventario cuadra (nuevas+usadas=totales).
 * El botón Confirmar está disponible desde el primer momento; no hace
 * falta esperar a que el peso se estabilice para poder pulsarlo. Incluye
 * también "Repetir pesada total" por si la pesada de la caja completa no
 * fue correcta.
 *
 * @param tiene_referencia   true si este código tiene fila en
 *        inventario_referencia.csv (ver csv_referencia.h); si es false, los
 *        otros dos parámetros se ignoran y ui_update_wait_weight_live() no
 *        muestra ninguna diferencia (ni roja ni verde) para este artículo.
 * @param referencia_nuevas  Stock teórico de nuevas para este código.
 * @param referencia_usadas  Stock teórico de usadas para este código.
 */
void ui_show_wait_weight_used(const char *descripcion, int unidades_totales, float peso_total_g,
                               bool tiene_referencia, int referencia_nuevas, int referencia_usadas);

/**
 * @brief Actualiza las unidades nuevas retiradas y usadas restantes (en
 * vivo, en grande, con un decimal) y el indicador de estable/inestable
 * (verde/rojo) de la pantalla de ui_show_wait_weight_used(). Se llama en
 * cada muestra de la báscula, sea o no estable, para dar feedback continuo.
 *
 * El decimal es a propósito: deja ver de un vistazo cómo de cerca está la
 * cuenta de un número entero, es decir, cómo de fiable es el
 * peso_unitario guardado para este artículo. Si @p peso_unitario_sospechoso
 * es true (el desvío supera la tolerancia), el número se resalta para
 * avisar de que puede haber un error de calibración o algo raro en la caja.
 *
 * Si la pantalla se abrió con referencia (ver ui_show_wait_weight_used()),
 * también actualiza, junto a cada número, la diferencia contra el stock
 * teórico (redondeado al entero más cercano): un check verde si cuadra, o
 * la diferencia en rojo si no. Sin referencia, ese hueco se deja en blanco.
 */
void ui_update_wait_weight_live(float unidades_nuevas, float unidades_usadas, bool stable,
                                 bool peso_unitario_sospechoso);

/**
 * @brief Pide vaciar la caja (retirar todos los utiles) y confirmar con el
 * botón táctil, con vigilancia en segundo plano igual que
 * ui_show_wait_weight_used(): no hace falta esperar a que el peso se
 * estabilice para poder pulsar Confirmar, y se puede pulsar tantas veces
 * como haga falta hasta que la caja este realmente vacia.
 */
void ui_show_wait_tare(void);

/**
 * @brief Igual que ui_show_wait_tare(), pero para la pesada por partes: lo
 * que se pesa aquí es el recipiente que se usará para ir contando tandas
 * (puede ser la propia caja o cualquier otro), no la caja del artículo.
 */
void ui_show_partial_tare(void);

/**
 * @brief Actualiza solo el texto de la última lectura de peso mostrada en
 * la pantalla de tara, sin tocar los botones. Sirve para las dos variantes
 * (ui_show_wait_tare() y ui_show_partial_tare()): reutiliza la instrucción
 * que dejó puesta la que se mostró por última vez.
 */
void ui_update_wait_tare_reading(float weight_g);

/** Aviso bajo el número de la tanda actual en ui_show_partial_count(). */
typedef enum {
    UI_PARTIAL_HINT_NONE,
    UI_PARTIAL_HINT_EMPTY_CONTAINER, /* tanda ya registrada: hay que vaciar antes de la siguiente */
    UI_PARTIAL_HINT_WAITING_STABLE,  /* NUEVAS/USADAS pulsado, esperando a que el peso se estabilice */
} ui_partial_hint_t;

/**
 * @brief Pantalla de recuento por tandas, para cajas que superan el máximo
 * de la báscula: se van pesando puñados y clasificándolos con los botones
 * NUEVAS/USADAS, que suman a sus contadores.
 *
 * Tres columnas: tanda actual (en vivo, con un decimal, igual que en
 * ui_show_wait_weight_used() y por el mismo motivo) y los dos acumulados.
 * Cinco botones: NUEVAS y USADAS (acción principal, grandes), y debajo
 * Deshacer (APP_EVT_UI_RETRY_PRESSED, resta la última tanda), Finalizar
 * (APP_EVT_UI_CONFIRM_PRESSED, guarda en inventario.csv) y Cancelar.
 *
 * Los parámetros de referencia funcionan igual que en
 * ui_show_wait_weight_used(): con fila en inventario_referencia.csv se
 * muestra la diferencia contra el teórico bajo cada acumulado.
 */
void ui_show_partial_count(const char *descripcion, bool tiene_referencia,
                            int referencia_nuevas, int referencia_usadas);

/**
 * @brief Refresca los números de ui_show_partial_count(): acumulados, tanda
 * actual y LED de estable/inestable.
 *
 * Con @p hint = UI_PARTIAL_HINT_EMPTY_CONTAINER la tanda actual se muestra
 * como "--" en vez de su valor: la tanda ya se ha registrado y lo que siga
 * marcando la báscula hasta que se vacíe el recipiente no es una tanda
 * nueva, así que enseñarlo solo invitaría a contarla dos veces.
 */
void ui_update_partial_count(int nuevas, int usadas, float tanda_uds,
                              ui_partial_hint_t hint, bool stable,
                              bool peso_unitario_sospechoso);

/** Peso usados > peso total: no se ha guardado nada, se puede reintentar
 *  la segunda pesada. */
void ui_show_inconsistent_weight(void);

/** Resultado guardado correctamente en inventario.csv. */
void ui_show_result_ok(const char *codigo, int unidades_nuevas, int unidades_usadas);

/**
 * @brief Confirmacion de que datos_maestros.csv se ha actualizado (boton
 * "Actualizar datos" desde ui_show_wait_weight_used()). El recuento que
 * estuviera en curso se descarta a proposito, sin tocar inventario.csv.
 */
void ui_show_result_master_updated(const char *codigo, const char *descripcion, float tara_caja, float peso_unitario);

/** Error fatal de arranque (SD o datos maestros): no hay botones, pantalla fija. */
void ui_show_fatal_error(const char *msg);

/**
 * @brief Teclado numérico en pantalla (disposición tipo numpad) para dar de
 * alta un material nuevo: últimos dígitos del código, unidades totales,
 * calibre y cabeza. También lo reutiliza la edición de ajustes.
 *
 * @param titulo          Texto de la pregunta (p.ej. "Ultimos digitos del codigo").
 * @param permitir_decimal Si true, la tecla "." está activa (para calibre/cabeza).
 * @param max_len         Longitud máxima de caracteres admitidos.
 * @param valor_inicial   Texto con el que arranca el visor (p.ej. el valor actual
 *                        de un ajuste, para corregirlo en vez de teclearlo entero).
 *                        NULL o "" para arrancar vacío (comportamiento de siempre).
 */
void ui_show_keypad(const char *titulo, bool permitir_decimal, int max_len, const char *valor_inicial);

/**
 * @brief Muestra el peso recién detectado (estable) y pide confirmarlo o
 * repetir la pesada. Se usa en el paso de peso total del alta de un
 * material nuevo, donde sí interesa poder descartar una lectura mal tomada
 * antes de seguir.
 *
 * @param titulo Texto de contexto (p.ej. "Peso total (material nuevo)").
 * @param peso_g Valor detectado, en gramos.
 */
void ui_show_confirm_weight(const char *titulo, float peso_g);

/**
 * @brief Pantalla del modo USB Mass Storage activo. Sin más salida que el
 * botón "Reiniciar" (visible en esta pantalla, además de en reposo).
 */
void ui_show_usb_mode(void);

/**
 * @brief Lista de los ajustes de báscula/pantalla, agrupados por sección,
 * con scroll (no caben los 8 a la vez). Cada fila muestra nombre + valor
 * actual; tocarla dispara APP_EVT_UI_SETTINGS_ROW_PRESSED con su id.
 * "Restablecer valores de fábrica" (contorno, deliberadamente pequeño y
 * discreto: es una acción rara y de las que no se deshacen fácil, no debe
 * competir en tamaño con las filas de uso normal) dispara
 * APP_EVT_UI_SETTINGS_RESET_PRESSED. "Volver" (esquina) es Cancelar.
 *
 * Hay que llamarla también al volver de editar/explicar un ajuste, para
 * refrescar el valor de la fila que se acaba de tocar.
 */
void ui_show_settings_list(void);

/**
 * @brief Pantalla intermedia antes de editar un ajuste: nombre, descripción
 * de qué hace y qué pasa al subirlo/bajarlo, valor actual y rango válido.
 * "Editar" (Confirmar) pasa al teclado; "Cancelar" vuelve a la lista.
 *
 * El título y el cuerpo pueden ocupar más o menos líneas según el ajuste;
 * los botones se anclan al borde real del texto (no a una coordenada fija)
 * para no solaparse nunca, sea cual sea la longitud.
 */
void ui_show_settings_explain(const char *nombre, const char *descripcion,
                               const char *valor_actual, const char *rango);

/**
 * @brief Confirmación antes de restablecer los 8 ajustes a fábrica (afecta
 * a todos a la vez, por eso pide confirmar). "Sí, restablecer" (Confirmar)
 * ejecuta el reset; "Cancelar" vuelve a la lista sin tocar nada.
 */
void ui_show_settings_reset_confirm(void);

#ifdef __cplusplus
}
#endif
