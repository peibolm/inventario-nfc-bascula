#include "app_fsm.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

#include "app_events.h"
#include "ui_screens.h"
#include "csv_master.h"
#include "csv_inventory.h"
#include "csv_referencia.h"
#include "scale_task.h"
#include "esp_bsp.h"
#include "display.h"
#include "usb_msc.h"
#include "settings.h"

static const char *TAG = "app_fsm";

/* Atenuacion de pantalla por inactividad: sin toques, tags leidos ni
 * lecturas de peso durante este tiempo, se baja el brillo. Cualquier
 * actividad lo restaura al instante. Tiempo/brillo son ajustables desde el
 * menu de Ajustes (ver settings.h); SCREEN_DIM_CHECK_MS es solo la
 * frecuencia interna de sondeo, no tiene sentido exponerla. */
#define SCREEN_DIM_CHECK_MS    1000

static bool s_screen_dimmed = false;

/* Constante para generar el codigo de un material nuevo: codigo = 1300000000
 * + los ultimos digitos (hasta 8) que teclee el operario. Da siempre un
 * codigo de 10 digitos empezando por "13". */
#define REG_CODIGO_BASE 1300000000L
#define REG_CODIGO_MAX_DIGITS 8

static const char *MSG_REG_WEIGH_TOTAL = "Material nuevo: coloque la caja completa y espere...";
static const char *MSG_UPDATE_WEIGH_TOTAL = "Actualizando datos maestros: coloque la caja completa y espere...";

typedef enum {
    APP_STATE_IDLE,
    APP_STATE_WAIT_WEIGHT_TOTAL,
    APP_STATE_WAIT_WEIGHT_USED,
    APP_STATE_ERROR_DISMISSABLE,  /* duplicado / codigo ya existente -> Aceptar vuelve a IDLE (o a reintroducir codigo) */
    APP_STATE_DUPLICATE_CONFIRM,  /* codigo ya en inventario.csv -> Aceptar vuelve a IDLE, Sobrescribir repite el flujo */
    APP_STATE_TIMEOUT_WEIGHT,     /* timeout de bascula -> Reintentar re-arma la misma pesada */
    APP_STATE_INCONSISTENT_WEIGHT, /* usadas > totales -> Reintentar re-arma la 2a pesada */
    APP_STATE_CONFIRM_WEIGHT,     /* peso recien detectado, pendiente de Confirmar/Repetir pesada */

    /* Alta de material nuevo (tag no encontrado en datos_maestros) */
    APP_STATE_REG_WAIT_WEIGHT_TOTAL, /* pesando la caja completa */
    APP_STATE_REG_ENTER_CODE,        /* teclado: ultimos digitos del codigo */
    APP_STATE_REG_CODE_DUPLICATE,    /* el codigo generado ya tiene OTRO tag vinculado (se perdio/cambio el
                                       * fisico?) -> Sobrescribir re-vincula este tag, Cancelar vuelve a reposo */
    APP_STATE_REG_CONFIRM_ARTICULO,  /* el codigo tecleado ya existe -> se muestra la descripcion a toda
                                       * pantalla para validar que es el material correcto ANTES de pedir
                                       * nada mas, evita contar con el codigo/material equivocado por un
                                       * error de tecleo o una caja mal etiquetada */
    APP_STATE_REG_ENTER_UNITS,       /* teclado: unidades totales (entero, manual) */
    APP_STATE_REG_WAIT_TARE,         /* pesando la caja vacia */
    APP_STATE_REG_ENTER_CALIBRE,     /* teclado decimal: calibre */
    APP_STATE_REG_ENTER_CABEZA,      /* teclado decimal: cabeza */

    /* Pesada por partes: para cajas que superan el maximo de la bascula, se
     * van pesando tandas sueltas y clasificandolas a mano (ver
     * handle_partial_mode_pressed()). Solo para articulos ya catalogados:
     * hace falta el peso_unitario de datos_maestros para convertir el peso
     * de cada tanda en unidades. */
    APP_STATE_PARTIAL_WAIT_TARE, /* pesando el recipiente vacio que se usara para las tandas */
    APP_STATE_PARTIAL_COUNT,     /* contando tandas con los botones NUEVAS/USADAS */

    /* Menu de ajustes (solo accesible desde reposo) */
    APP_STATE_SETTINGS_LIST,          /* lista de los 8 ajustes */
    APP_STATE_SETTINGS_EXPLAIN,       /* descripcion/rango de un ajuste, antes de editarlo */
    APP_STATE_SETTINGS_EDIT,          /* teclado, editando el ajuste seleccionado */
    APP_STATE_SETTINGS_RESET_CONFIRM, /* confirmacion de "Restablecer valores de fabrica" */
} app_state_t;

/* Boton de clasificar pulsado en la pesada por partes mientras el peso aun
 * no estaba estable: se recuerda y la tanda se registra sola en cuanto
 * llegue el peso estable (ver handle_partial_button()). */
typedef enum {
    PARCIAL_PENDIENTE_NINGUNA,
    PARCIAL_PENDIENTE_NUEVAS,
    PARCIAL_PENDIENTE_USADAS,
} parcial_pendiente_t;

typedef struct {
    char codigo[MASTER_CODIGO_MAX_LEN];
    char descripcion[MASTER_DESC_MAX_LEN];
    float tara_caja;
    float peso_unitario;
    int unidades_totales;
    float peso_total_g;                /* peso bruto de la caja completa, para mostrarlo en pantalla */
    bool overwrite_duplicate;         /* true si se confirmo sobrescribir un codigo ya en inventario.csv */
    app_state_t pending_weight_state; /* a que pesada volver tras un timeout o al confirmar/repetir */
    float pending_weight_g;           /* peso detectado, pendiente de confirmar (REG_*) */
    bool tiene_lectura_usados;        /* ya llego alguna lectura de fondo en WAIT_WEIGHT_USED */
    bool tiene_lectura_tara;          /* ya llego alguna lectura de fondo en REG_WAIT_TARE */

    /* Campos temporales, solo usados durante el alta de un material nuevo
     * o la actualizacion de uno ya existente (mismo tramo final,
     * unidades -> tara -> calibre -> cabeza, ver reg_is_update) */
    char reg_uid[MASTER_UID_MAX_LEN];
    float reg_peso_total;
    float reg_calibre;
    /* true si este recorrido del tramo REG_* es para CORREGIR codigo/
     * descripcion/tara/peso_unitario ya existentes (boton "Actualizar
     * datos" desde ui_show_wait_weight_used), no para dar de alta un
     * material nuevo - cambia el destino final (csv_master_update_by_codigo
     * en vez de csv_master_append) y se salta pedir el codigo, que ya se
     * conoce. */
    bool reg_is_update;
    /* true si el tag que se esta registrando (nuevo, uid no encontrado)
     * corresponde a un codigo YA EXISTENTE en datos_maestros al que le
     * falta tara y/o peso_unitario ("Caso B": precatalogado a medias, con
     * codigo+descripcion pero sin completar). Igual que reg_is_update, se
     * salta calibre/cabeza y se actualiza la fila en vez de anadir una
     * nueva - pero ADEMAS hay que vincular el tag al terminar (por eso no
     * comparten el mismo flag ni la misma funcion de cierre), y el destino
     * final es continuar el inventariado (WAIT_WEIGHT_USED), no volver a
     * reposo. */
    bool reg_completando_existente;

    /* Ajuste seleccionado en la lista de Ajustes, mientras se explica/edita. */
    setting_id_t settings_selected;

    /* Stock teorico del codigo actual, si tiene fila en
     * inventario_referencia.csv (ver show_wait_weight_used()). */
    bool tiene_referencia;
    int referencia_nuevas;
    int referencia_usadas;

    /* Pesada por partes (APP_STATE_PARTIAL_*) */
    int parcial_nuevas;             /* acumulado de tandas clasificadas como nuevas */
    int parcial_usadas;             /* idem, usadas */
    float parcial_tara_g;           /* tara del recipiente, medida al entrar en el flujo */
    float parcial_peso_tanda_g;     /* ultimo peso estable leido (bruto, sin restar tara) */
    bool parcial_tanda_lista;       /* ese peso estable corresponde a una tanda aun sin registrar */
    bool parcial_esperando_vaciado; /* tanda ya registrada: no se admite otra hasta vaciar el recipiente */
    parcial_pendiente_t parcial_pendiente; /* NUEVAS/USADAS pulsado sin peso estable todavia */
    /* Ultima tanda registrada, para Deshacer (un solo nivel) */
    bool parcial_hay_ultima;
    int parcial_ultima_uds;
    bool parcial_ultima_era_nuevas;
    float parcial_ultima_peso_g;
} process_ctx_t;

static app_state_t s_state = APP_STATE_IDLE;
static process_ctx_t s_ctx;

/* Recoge los ultimos articulos registrados (codigo->descripcion via
 * datos_maestros) y refresca la pantalla de reposo con esa tabla. */
static void go_idle(void)
{
    s_state = APP_STATE_IDLE;
    scale_cancel_weighing();
    memset(&s_ctx, 0, sizeof(s_ctx));

    csv_inventory_recent_t recent[CSV_INVENTORY_RECENT_MAX];
    size_t recent_count = csv_inventory_get_recent(recent, CSV_INVENTORY_RECENT_MAX);

    ui_recent_item_t ui_items[UI_RECENT_MAX];
    for (size_t i = 0; i < recent_count; i++) {
        const master_item_t *item = csv_master_find_by_codigo(recent[i].codigo);
        if (item) {
            strlcpy(ui_items[i].descripcion, item->descripcion, sizeof(ui_items[i].descripcion));
        } else {
            strlcpy(ui_items[i].descripcion, recent[i].codigo, sizeof(ui_items[i].descripcion));
        }
        ui_items[i].unidades_nuevas = recent[i].unidades_nuevas;
        ui_items[i].unidades_usadas = recent[i].unidades_usadas;
    }
    ui_show_idle(ui_items, recent_count);
}

static void finish_used_weighing(float weight_g);
static void finish_master_update(void);
static void finish_master_completion(void);
static void link_tag_and_proceed(const master_item_t *existing);
static void route_existing_articulo(const master_item_t *existing);
static void partial_refresh_ui(void);
static void partial_live_update(float weight_g, bool stable);
static void partial_register_batch(bool es_nuevas);

/* Busca el stock teorico de s_ctx.codigo en inventario_referencia.csv (si
 * el fichero existe y el codigo figura en el) y muestra la pantalla de
 * retirar nuevos/usados. Es un asistente opcional para detectar posibles
 * errores de conteo: sin fichero o sin ese codigo, el recuento sigue
 * funcionando exactamente igual, solo sin comparar contra ningun teorico. */
static void show_wait_weight_used(void)
{
    const referencia_item_t *ref = csv_referencia_find_by_codigo(s_ctx.codigo);
    s_ctx.tiene_referencia = (ref != NULL);
    s_ctx.referencia_nuevas = ref ? ref->unidades_nuevas : 0;
    s_ctx.referencia_usadas = ref ? ref->unidades_usadas : 0;

    ui_show_wait_weight_used(s_ctx.descripcion, s_ctx.unidades_totales, s_ctx.peso_total_g,
                              s_ctx.tiene_referencia, s_ctx.referencia_nuevas, s_ctx.referencia_usadas);
}

/* Igual que show_wait_weight_used(), pero para la pantalla de recuento por
 * tandas: mismo asistente opcional de stock teorico, comparado esta vez
 * contra los acumulados que se van sumando. */
static void show_partial_count(void)
{
    const referencia_item_t *ref = csv_referencia_find_by_codigo(s_ctx.codigo);
    s_ctx.tiene_referencia = (ref != NULL);
    s_ctx.referencia_nuevas = ref ? ref->unidades_nuevas : 0;
    s_ctx.referencia_usadas = ref ? ref->unidades_usadas : 0;

    ui_show_partial_count(s_ctx.descripcion, s_ctx.tiene_referencia,
                           s_ctx.referencia_nuevas, s_ctx.referencia_usadas);
}

/* Arma la pesada aplicando el filtro de peso minimo solo cuando el estado
 * de destino es una pesada de "caja completa" (normal o de alta de
 * material nuevo); el resto de pesadas (tara, usados) no se filtran. */
static void start_weighing_for_state(app_state_t target_state)
{
    float min_g = (target_state == APP_STATE_WAIT_WEIGHT_TOTAL ||
                    target_state == APP_STATE_REG_WAIT_WEIGHT_TOTAL)
                       ? settings_get(SETTING_SCALE_MIN_TOTAL_WEIGHT_G)
                       : 0.0f;
    scale_request_weighing(min_g);
}

/* Igual que start_weighing_for_state(), pero sin reiniciar la ventana de
 * estabilidad ya acumulada: para los re-armados propios de la vigilancia
 * en segundo plano (WAIT_WEIGHT_USED, REG_WAIT_TARE), donde un reinicio en
 * cada ciclo haria parpadear el indicador de estable aunque el peso real
 * no cambie. */
static void continue_weighing_for_state(app_state_t target_state)
{
    float min_g = (target_state == APP_STATE_WAIT_WEIGHT_TOTAL ||
                    target_state == APP_STATE_REG_WAIT_WEIGHT_TOTAL)
                       ? settings_get(SETTING_SCALE_MIN_TOTAL_WEIGHT_G)
                       : 0.0f;
    scale_continue_weighing(min_g);
}

/* Marca actividad "no tactil" (tag leido, peso detectado) para que la
 * pantalla no se atenue mientras hay progreso real aunque nadie la toque. */
static void mark_activity(void)
{
    bsp_display_lock(0);
    lv_disp_trig_activity(NULL);
    bsp_display_unlock();
}

/* Comprueba el tiempo de inactividad (toques + mark_activity()) y atenua o
 * restaura el brillo de la pantalla en consecuencia. */
static void check_screen_dim(void)
{
    bsp_display_lock(0);
    uint32_t idle_ms = lv_disp_get_inactive_time(NULL);
    bsp_display_unlock();

    uint32_t timeout_ms = (uint32_t)(settings_get(SETTING_SCREEN_DIM_TIMEOUT_MIN) * 60000.0f);
    if (!s_screen_dimmed && idle_ms >= timeout_ms) {
        bsp_display_brightness_set((uint8_t)settings_get(SETTING_SCREEN_DIM_BRIGHTNESS_PCT));
        s_screen_dimmed = true;
    } else if (s_screen_dimmed && idle_ms < timeout_ms) {
        bsp_display_brightness_set(100);
        s_screen_dimmed = false;
    }
}

/* Unidades SIN REDONDEAR a partir del peso: sirve tanto para el resultado
 * final (unidades_from_weight() redondea esto al mas proximo, con
 * lroundf) como para mostrar en vivo cuantos decimales de margen hay -
 * cuanto mas cerca de un entero, mas fiable es el peso_unitario guardado
 * en datos_maestros para este articulo. */
static float units_from_weight_f(float peso_bruto_g)
{
    float peso_neto = peso_bruto_g - s_ctx.tara_caja;
    if (peso_neto < 0.0f) {
        peso_neto = 0.0f;
    }
    if (s_ctx.peso_unitario <= 0.0f) {
        return 0.0f;
    }
    return peso_neto / s_ctx.peso_unitario;
}

static int units_from_weight(float peso_bruto_g)
{
    return (int)lroundf(units_from_weight_f(peso_bruto_g));
}

static void handle_nfc_tag(const char *uid_hex)
{
    mark_activity();

    if (s_state != APP_STATE_IDLE) {
        ESP_LOGD(TAG, "Tag ignorado, proceso en curso (uid=%s)", uid_hex);
        return;
    }

    const master_item_t *item = csv_master_find_by_uid(uid_hex);
    if (!item) {
        /* Material desconocido: en vez de error, arrancamos el alta guiada */
        ESP_LOGI(TAG, "UID no encontrado, iniciando alta de material nuevo: %s", uid_hex);
        memset(&s_ctx, 0, sizeof(s_ctx));
        strlcpy(s_ctx.reg_uid, uid_hex, sizeof(s_ctx.reg_uid));

        s_state = APP_STATE_REG_WAIT_WEIGHT_TOTAL;
        ui_show_description_and_wait_weight(MSG_REG_WEIGH_TOTAL, false);
        start_weighing_for_state(s_state);
        return;
    }

    if (csv_inventory_has_codigo(item->codigo)) {
        ESP_LOGW(TAG, "Codigo duplicado: %s", item->codigo);
        memset(&s_ctx, 0, sizeof(s_ctx));
        strlcpy(s_ctx.codigo, item->codigo, sizeof(s_ctx.codigo));
        strlcpy(s_ctx.descripcion, item->descripcion, sizeof(s_ctx.descripcion));
        s_ctx.tara_caja = item->tara_caja;
        s_ctx.peso_unitario = item->peso_unitario;

        s_state = APP_STATE_DUPLICATE_CONFIRM;
        ui_show_duplicate_warning(item->descripcion);
        return;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.codigo, item->codigo, sizeof(s_ctx.codigo));
    strlcpy(s_ctx.descripcion, item->descripcion, sizeof(s_ctx.descripcion));
    s_ctx.tara_caja = item->tara_caja;
    s_ctx.peso_unitario = item->peso_unitario;

    s_state = APP_STATE_WAIT_WEIGHT_TOTAL;
    ui_show_description_and_wait_weight(s_ctx.descripcion, true);
    start_weighing_for_state(s_state);
}

/* Termina el alta de un material nuevo: guarda la entrada en datos_maestros
 * (RAM + fichero) y continua con el tramo final del flujo habitual
 * (confirmar retirada de nuevos -> segunda pesada), reutilizando s_ctx
 * exactamente igual que si el articulo ya existiera de antes. */
static void finish_new_material_registration(void)
{
    master_item_t nuevo = {0};
    strlcpy(nuevo.uid_nfc, s_ctx.reg_uid, sizeof(nuevo.uid_nfc));
    strlcpy(nuevo.codigo, s_ctx.codigo, sizeof(nuevo.codigo));
    strlcpy(nuevo.descripcion, s_ctx.descripcion, sizeof(nuevo.descripcion));
    nuevo.tara_caja = s_ctx.tara_caja;
    nuevo.peso_unitario = s_ctx.peso_unitario;

    if (csv_master_append(&nuevo) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar el material nuevo en datos_maestros.csv");
    }

    s_state = APP_STATE_WAIT_WEIGHT_USED;
    s_ctx.tiene_lectura_usados = false;
    s_ctx.peso_total_g = s_ctx.reg_peso_total;
    show_wait_weight_used();
    start_weighing_for_state(s_state);
}

/* Arranca desde ui_show_wait_weight_used() ("Actualizar datos"): aborta a
 * proposito el recuento en curso (no se guarda nada en inventario.csv) y
 * reutiliza el tramo final del alta de material nuevo para volver a pesar
 * la caja llena, pedir unidades reales, pesar la tara vacia y recalcular
 * peso_unitario desde cero - necesario porque las "unidades" que ya
 * calculo la bascula en WAIT_WEIGHT_TOTAL se hicieron con el
 * peso_unitario ANTIGUO, que puede ser precisamente el dato erroneo. El
 * codigo no se vuelve a pedir: ya se conoce (s_ctx.codigo no se toca). */
static void handle_update_master_pressed(void)
{
    if (s_state != APP_STATE_WAIT_WEIGHT_USED) {
        return;
    }

    scale_cancel_weighing();

    s_ctx.reg_is_update = true;
    s_ctx.tiene_lectura_usados = false;

    s_state = APP_STATE_REG_WAIT_WEIGHT_TOTAL;
    ui_show_description_and_wait_weight(MSG_UPDATE_WEIGH_TOTAL, false);
    start_weighing_for_state(s_state);
}

/* Termina la actualizacion de una entrada YA EXISTENTE en datos_maestros
 * (boton "Actualizar datos"): sustituye descripcion/tara/peso_unitario en
 * la fila de s_ctx.codigo (el uid_nfc no cambia) y vuelve a reposo SIN
 * guardar nada en inventario.csv - el recuento se descarto a proposito al
 * pulsar el boton, hay que reescanear el tag para inventariar con los
 * datos ya corregidos. */
static void finish_master_update(void)
{
    const master_item_t *existing = csv_master_find_by_codigo(s_ctx.codigo);

    master_item_t actualizado = {0};
    if (existing) {
        strlcpy(actualizado.uid_nfc, existing->uid_nfc, sizeof(actualizado.uid_nfc));
    }
    strlcpy(actualizado.codigo, s_ctx.codigo, sizeof(actualizado.codigo));
    strlcpy(actualizado.descripcion, s_ctx.descripcion, sizeof(actualizado.descripcion));
    actualizado.tara_caja = s_ctx.tara_caja;
    actualizado.peso_unitario = s_ctx.peso_unitario;

    if (csv_master_update_by_codigo(&actualizado) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo actualizar datos_maestros.csv para %s", s_ctx.codigo);
    }

    ui_show_result_master_updated(s_ctx.codigo, s_ctx.descripcion, s_ctx.tara_caja, s_ctx.peso_unitario);

    vTaskDelay(pdMS_TO_TICKS(2500));
    go_idle();
}

/* Termina de rellenar un codigo precatalogado al que le faltaba tara y/o
 * peso_unitario ("Caso B"): guarda esos datos (mas el UID del tag que se
 * esta registrando, que hasta ahora no tenia ninguno) en datos_maestros, y
 * CONTINUA el inventariado igual que un articulo ya completo -a
 * diferencia de finish_master_update(), esto no viene de corregir un dato
 * ya bueno sino de completar uno que faltaba, asi que no tiene sentido
 * descartar el recuento: se seguia con el mismo tag/caja que se acaba de
 * pesar. */
static void finish_master_completion(void)
{
    master_item_t completado = {0};
    strlcpy(completado.uid_nfc, s_ctx.reg_uid, sizeof(completado.uid_nfc));
    strlcpy(completado.codigo, s_ctx.codigo, sizeof(completado.codigo));
    strlcpy(completado.descripcion, s_ctx.descripcion, sizeof(completado.descripcion));
    completado.tara_caja = s_ctx.tara_caja;
    completado.peso_unitario = s_ctx.peso_unitario;

    if (csv_master_update_by_codigo(&completado) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo completar datos_maestros.csv para %s", s_ctx.codigo);
    }

    ESP_LOGI(TAG, "Precatalogado %s completado: tara=%.2fg peso_unit=%.4fg uid=%s, unidades totales=%d",
             s_ctx.codigo, s_ctx.tara_caja, s_ctx.peso_unitario, s_ctx.reg_uid, s_ctx.unidades_totales);

    s_state = APP_STATE_WAIT_WEIGHT_USED;
    s_ctx.tiene_lectura_usados = false;
    s_ctx.peso_total_g = s_ctx.reg_peso_total;
    show_wait_weight_used();
    start_weighing_for_state(s_state);
}

/* Vincula el tag que se esta registrando (s_ctx.reg_uid) a una entrada YA
 * EXISTENTE de datos_maestros (descripcion/tara/peso_unitario conocidos de
 * antemano) y continua directo a la vigilancia de retirada, igual que un
 * articulo ya catalogado. Dos llamadores:
 *  - Material precatalogado (fila sin tag aun vinculado, uid_nfc vacio).
 *  - Re-vinculacion tras confirmar "Sobrescribir" en
 *    APP_STATE_REG_CODE_DUPLICATE (el codigo ya tenia OTRO tag vinculado,
 *    p.ej. porque se perdio/rompio el fisico anterior y se le pego uno
 *    nuevo a la caja) - csv_master_link_uid() sustituye el UID sin mas,
 *    sea cual sea el que hubiera antes. */
static void link_tag_and_proceed(const master_item_t *existing)
{
    strlcpy(s_ctx.descripcion, existing->descripcion, sizeof(s_ctx.descripcion));
    s_ctx.tara_caja = existing->tara_caja;
    s_ctx.peso_unitario = existing->peso_unitario;

    if (csv_master_link_uid(s_ctx.codigo, s_ctx.reg_uid) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo vincular el tag %s al codigo %s", s_ctx.reg_uid, s_ctx.codigo);
    }

    s_ctx.unidades_totales = units_from_weight(s_ctx.reg_peso_total);
    ESP_LOGI(TAG, "Tag vinculado a codigo %s, unidades totales=%d", s_ctx.codigo, s_ctx.unidades_totales);

    s_state = APP_STATE_WAIT_WEIGHT_USED;
    s_ctx.tiene_lectura_usados = false;
    s_ctx.peso_total_g = s_ctx.reg_peso_total;
    show_wait_weight_used();
    start_weighing_for_state(s_state);
}

/* Se llama tras confirmar en APP_STATE_REG_CONFIRM_ARTICULO (el operario ya
 * ha validado que la descripcion es la del material que tiene delante).
 * A partir de aqui, decide Caso A (todo completo, solo vincular) o Caso B
 * (falta tara y/o peso_unitario, hay que completarlo) - s_ctx.descripcion/
 * tara_caja/peso_unitario ya estan puestos desde que se encontro el
 * codigo. */
static void route_existing_articulo(const master_item_t *existing)
{
    bool falta_tara = s_ctx.tara_caja <= 0.0f;
    bool falta_peso_unitario = s_ctx.peso_unitario <= 0.0f;

    if (!falta_tara && !falta_peso_unitario) {
        /* Caso A: ya esta todo, solo hace falta vincular el tag. */
        link_tag_and_proceed(existing);
        return;
    }

    /* Caso B: falta tara y/o peso_unitario. Se completa lo que falte,
     * reutilizando el mismo tramo de pesada del alta nueva, pero sin tocar
     * la descripcion ni pedir calibre/cabeza, y actualizando la fila en
     * vez de anadir una nueva (ver finish_master_completion()). */
    s_ctx.reg_completando_existente = true;

    if (falta_peso_unitario) {
        /* Sin peso_unitario no se puede saber cuantas unidades hay con
         * solo el peso total: hacen falta las unidades reales, tecleadas a
         * mano. Si ADEMAS falta la tara, tras teclearlas se pedira pesar
         * la caja vacia (ver REG_ENTER_UNITS); si la tara ya se conocia,
         * se calcula ya mismo sin pesar nada mas (ver REG_ENTER_UNITS). */
        s_state = APP_STATE_REG_ENTER_UNITS;
        ui_show_keypad("Unidades totales en la caja", false, 5, NULL);
    } else {
        /* Solo falta la tara, el peso_unitario ya se conocia: basta con
         * pesar la caja vacia, las unidades salen solas de ahi (ver
         * REG_WAIT_TARE en handle_confirm_pressed). */
        s_state = APP_STATE_REG_WAIT_TARE;
        s_ctx.tiene_lectura_tara = false;
        ui_show_wait_tare();
        start_weighing_for_state(s_state);
    }
}

static void handle_scale_stable(float weight_g)
{
    mark_activity();

    switch (s_state) {
    case APP_STATE_WAIT_WEIGHT_TOTAL:
        /* Articulo ya conocido: se acepta automaticamente, sin confirmar, y
         * se pasa directo a vigilar la retirada de utiles nuevos. */
        s_ctx.unidades_totales = units_from_weight(weight_g);
        ESP_LOGI(TAG, "Unidades totales: %d", s_ctx.unidades_totales);
        s_state = APP_STATE_WAIT_WEIGHT_USED;
        s_ctx.tiene_lectura_usados = false;
        s_ctx.peso_total_g = weight_g;
        show_wait_weight_used();
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_WAIT_WEIGHT_USED:
        /* v2 "sin botones": si la bascula se queda a 0 (caja retirada por
         * completo), se usa el ultimo peso estable ya visto como resultado
         * final, igual que si se hubiera pulsado Confirmar justo antes de
         * levantar la caja. Si aun no hay ninguna lectura previa (caja
         * levantada nada mas empezar, antes de estabilizar), se ignora y
         * se sigue esperando. Confirmar sigue disponible como respaldo
         * manual en todo momento. */
        if (fabsf(weight_g) <= settings_get(SETTING_SCALE_ZERO_THRESHOLD_G)) {
            if (s_ctx.tiene_lectura_usados) {
                finish_used_weighing(s_ctx.pending_weight_g);
            } else {
                continue_weighing_for_state(APP_STATE_WAIT_WEIGHT_USED);
            }
            break;
        }

        /* Vigilancia en segundo plano: se actualiza el valor mostrado pero
         * no se avanza de estado hasta que el operario pulse Confirmar (o
         * la bascula llegue a 0), pudiendo retirar los utiles nuevos en
         * varias tandas. Se vuelve a armar para seguir mirando (sin
         * reiniciar la ventana, para que el indicador de estable no
         * parpadee si el peso no ha cambiado). */
        s_ctx.pending_weight_g = weight_g;
        s_ctx.tiene_lectura_usados = true;
        continue_weighing_for_state(APP_STATE_WAIT_WEIGHT_USED);
        break;

    case APP_STATE_REG_WAIT_WEIGHT_TOTAL:
        s_ctx.pending_weight_g = weight_g;
        s_ctx.pending_weight_state = s_state;
        s_state = APP_STATE_CONFIRM_WEIGHT;
        ui_show_confirm_weight(s_ctx.reg_is_update ? "Peso total (actualizar datos)" : "Peso total (material nuevo)",
                               weight_g);
        break;

    case APP_STATE_REG_WAIT_TARE:
    case APP_STATE_PARTIAL_WAIT_TARE:
        /* Igual que WAIT_WEIGHT_USED: vigilancia en segundo plano sin
         * avanzar de estado, para dar tiempo real a vaciar la caja en vez
         * de aceptar el primer peso estable (que normalmente es aun el de
         * la caja llena, recien salida del paso anterior). */
        s_ctx.pending_weight_g = weight_g;
        s_ctx.tiene_lectura_tara = true;
        ui_update_wait_tare_reading(weight_g);
        continue_weighing_for_state(s_state);
        break;

    case APP_STATE_PARTIAL_COUNT:
        if (s_ctx.parcial_esperando_vaciado) {
            /* La tanda anterior ya esta contada: solo se mira si el
             * recipiente ha vuelto a estar vacio (o se ha retirado de la
             * bascula, que da un neto muy negativo) para admitir la
             * siguiente. */
            if ((weight_g - s_ctx.parcial_tara_g) <= settings_get(SETTING_SCALE_ZERO_THRESHOLD_G)) {
                s_ctx.parcial_esperando_vaciado = false;
                partial_refresh_ui();
            }
        } else {
            s_ctx.parcial_peso_tanda_g = weight_g;
            s_ctx.parcial_tanda_lista = true;
            if (s_ctx.parcial_pendiente != PARCIAL_PENDIENTE_NINGUNA) {
                /* NUEVAS/USADAS se pulso antes de que el peso estuviera
                 * estable: ya lo esta, se registra ahora. */
                partial_register_batch(s_ctx.parcial_pendiente == PARCIAL_PENDIENTE_NUEVAS);
            } else {
                partial_refresh_ui();
            }
        }
        continue_weighing_for_state(APP_STATE_PARTIAL_COUNT);
        break;

    default:
        ESP_LOGD(TAG, "Evento de peso estable ignorado en estado %d", (int)s_state);
        break;
    }
}

/* Lectura "en vivo" (cada muestra, estable o no) mientras se retiran
 * utiles nuevos: solo actualiza el feedback en pantalla (LED y
 * nuevas/usadas en grande); no cambia de estado ni guarda nada. Nuevas y
 * usadas siempre suman el total, para que se vea de un vistazo que el
 * inventario cuadra. */
static void handle_scale_reading(float weight_g, bool stable)
{
    if (s_state == APP_STATE_PARTIAL_COUNT) {
        partial_live_update(weight_g, stable);
        return;
    }

    if (s_state != APP_STATE_WAIT_WEIGHT_USED) {
        return;
    }

    /* Sin redondear todavia y SIN acotar a [0, total]: el aviso de
     * peso_unitario sospechoso mira el desvio real, antes de que el
     * recorte a los limites lo pueda enmascarar. */
    float unidades_nuevas_f = (float)s_ctx.unidades_totales - units_from_weight_f(weight_g);
    bool peso_unitario_sospechoso =
        fabsf(unidades_nuevas_f - roundf(unidades_nuevas_f)) >
        (settings_get(SETTING_UNIT_ROUNDING_TOLERANCE_PCT) / 100.0f);

    if (unidades_nuevas_f < 0.0f) {
        unidades_nuevas_f = 0.0f;
    } else if (unidades_nuevas_f > (float)s_ctx.unidades_totales) {
        unidades_nuevas_f = (float)s_ctx.unidades_totales;
    }
    float unidades_usadas_f = (float)s_ctx.unidades_totales - unidades_nuevas_f;

    ui_update_wait_weight_live(unidades_nuevas_f, unidades_usadas_f, stable, peso_unitario_sospechoso);
}

static void handle_scale_timeout(void)
{
    if (s_state == APP_STATE_WAIT_WEIGHT_USED || s_state == APP_STATE_REG_WAIT_TARE ||
        s_state == APP_STATE_PARTIAL_WAIT_TARE || s_state == APP_STATE_PARTIAL_COUNT) {
        /* No interrumpir al operario mientras retira utiles/vacia la caja
         * en varias tandas: se sigue vigilando en segundo plano sin
         * mostrar error. */
        ESP_LOGD(TAG, "Timeout de bascula en estado %d, se sigue vigilando", (int)s_state);
        continue_weighing_for_state(s_state);
        return;
    }

    if (s_state == APP_STATE_WAIT_WEIGHT_TOTAL || s_state == APP_STATE_REG_WAIT_WEIGHT_TOTAL) {
        s_ctx.pending_weight_state = s_state;
        s_state = APP_STATE_TIMEOUT_WEIGHT;
        ui_show_error("No se pudo leer un peso estable. Reintente.");
    }
}

/* Guarda el recuento en inventario.csv, lo confirma en pantalla y vuelve a
 * reposo. Comun a los dos finales posibles: la pesada normal (caja entera,
 * finish_used_weighing) y el recuento por tandas (finish_partial_counting). */
static void save_inventory_and_finish(int unidades_nuevas, int unidades_usadas)
{
    if (s_ctx.overwrite_duplicate) {
        /* Sobrescribir un codigo ya registrado obliga a reescribir
         * inventario.csv entero; con el fichero ya crecido eso bloquea la
         * pantalla lo suficiente como para que parezca colgada. Se avisa
         * antes, y se cede el paso 100ms para que la tarea de dibujo de
         * LVGL (prioridad 4, por debajo de esta maquina de estados, que va
         * a 5) llegue a pintar el aviso ANTES de que empiece la escritura:
         * sin esa pausa el mensaje puede aparecer cuando ya ha terminado,
         * que es justo cuando ya no sirve. LVGL refresca cada 30ms. */
        if (csv_inventory_has_codigo(s_ctx.codigo)) {
            ui_show_saving();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        csv_inventory_append_or_update(s_ctx.codigo, unidades_nuevas, unidades_usadas);
    } else {
        csv_inventory_append(s_ctx.codigo, unidades_nuevas, unidades_usadas);
    }
    ui_show_result_ok(s_ctx.codigo, unidades_nuevas, unidades_usadas);

    vTaskDelay(pdMS_TO_TICKS(2500));
    go_idle();
}

/* Calcula unidades usadas/nuevas a partir del peso ya confirmado por el
 * operario y guarda el resultado en inventario.csv. */
static void finish_used_weighing(float weight_g)
{
    scale_cancel_weighing(); /* dejar de vigilar en segundo plano */

    int unidades_usadas = units_from_weight(weight_g);
    ESP_LOGI(TAG, "Unidades usadas (confirmado por operario): %d", unidades_usadas);

    if (unidades_usadas > s_ctx.unidades_totales) {
        s_state = APP_STATE_INCONSISTENT_WEIGHT;
        ui_show_inconsistent_weight();
        return;
    }

    save_inventory_and_finish(s_ctx.unidades_totales - unidades_usadas, unidades_usadas);
}

/* ---- Pesada por partes -------------------------------------------------
 *
 * Para cajas que pesan mas de lo que admite la bascula: en vez de una sola
 * pesada de la caja entera, se van pesando tandas sueltas en un recipiente
 * (cuya tara se mide al entrar) y se clasifica cada una con NUEVAS/USADAS.
 * El total del articulo acaba siendo la suma de las tandas, no una lectura
 * unica, asi que aqui no hay comprobacion de "usadas > totales" que hacer:
 * el total ES nuevas+usadas por construccion.
 */

/* Unidades de la tanda que hay ahora mismo en el recipiente. Como la tara
 * es la del recipiente de tandas (medida en este flujo), no la de la caja
 * del articulo, no se puede usar units_from_weight(). */
static float partial_units_f(float peso_bruto_g)
{
    float peso_neto = peso_bruto_g - s_ctx.parcial_tara_g;
    if (peso_neto < 0.0f) {
        peso_neto = 0.0f;
    }
    if (s_ctx.peso_unitario <= 0.0f) {
        return 0.0f;
    }
    return peso_neto / s_ctx.peso_unitario;
}

static ui_partial_hint_t partial_current_hint(void)
{
    if (s_ctx.parcial_esperando_vaciado) {
        return UI_PARTIAL_HINT_EMPTY_CONTAINER;
    }
    if (s_ctx.parcial_pendiente != PARCIAL_PENDIENTE_NINGUNA) {
        return UI_PARTIAL_HINT_WAITING_STABLE;
    }
    return UI_PARTIAL_HINT_NONE;
}

/* Mismo aviso que en la pesada normal: cuanto se aleja la cuenta de un
 * numero entero, es decir, si el peso_unitario guardado cuadra o no. */
static bool partial_unit_weight_suspicious(float unidades_f)
{
    return fabsf(unidades_f - roundf(unidades_f)) >
           (settings_get(SETTING_UNIT_ROUNDING_TOLERANCE_PCT) / 100.0f);
}

/* Repinta los numeros de la pantalla de tandas tras un cambio de estado
 * (tanda registrada, deshecha, recipiente vaciado...). El refresco continuo
 * mientras el peso se mueve lo hace partial_live_update(). */
static void partial_refresh_ui(void)
{
    float tanda_f = s_ctx.parcial_tanda_lista ? partial_units_f(s_ctx.parcial_peso_tanda_g) : 0.0f;
    ui_update_partial_count(s_ctx.parcial_nuevas, s_ctx.parcial_usadas, tanda_f,
                             partial_current_hint(), s_ctx.parcial_tanda_lista,
                             s_ctx.parcial_tanda_lista && partial_unit_weight_suspicious(tanda_f));
}

/* Lectura "en vivo" (cada muestra, estable o no) mientras se cuentan
 * tandas: numero de la tanda actual y LED, sin tocar los acumulados. */
static void partial_live_update(float weight_g, bool stable)
{
    /* En cuanto la bascula se mueve, el ultimo peso estable deja de valer:
     * si no se invalidara aqui, pulsar NUEVAS/USADAS justo despues de echar
     * mas piezas registraria la tanda ANTERIOR (el ultimo valor estable) en
     * vez de esperar a que se asiente la de ahora. */
    if (!stable && !s_ctx.parcial_esperando_vaciado) {
        s_ctx.parcial_tanda_lista = false;
    }

    float tanda_f = partial_units_f(weight_g);
    ui_update_partial_count(s_ctx.parcial_nuevas, s_ctx.parcial_usadas, tanda_f,
                             partial_current_hint(), stable,
                             partial_unit_weight_suspicious(tanda_f));
}

/* Entrar en el flujo por partes desde la pantalla de peso total ("Pesar por
 * partes"). Lo primero es la tara del recipiente: sin ella no se puede
 * convertir el peso de ninguna tanda en unidades. */
static void handle_partial_mode_pressed(void)
{
    scale_cancel_weighing();

    s_ctx.parcial_nuevas = 0;
    s_ctx.parcial_usadas = 0;
    s_ctx.parcial_tara_g = 0.0f;
    s_ctx.parcial_peso_tanda_g = 0.0f;
    s_ctx.parcial_tanda_lista = false;
    s_ctx.parcial_esperando_vaciado = false;
    s_ctx.parcial_pendiente = PARCIAL_PENDIENTE_NINGUNA;
    s_ctx.parcial_hay_ultima = false;
    s_ctx.tiene_lectura_tara = false;

    ESP_LOGI(TAG, "Pesada por partes para %s: pidiendo tara del recipiente", s_ctx.codigo);

    s_state = APP_STATE_PARTIAL_WAIT_TARE;
    ui_show_partial_tare();
    start_weighing_for_state(s_state);
}

/* Pasa la tanda que hay en la bascula al contador correspondiente. Solo se
 * llama con un peso estable ya medido (parcial_tanda_lista). */
static void partial_register_batch(bool es_nuevas)
{
    int uds = (int)lroundf(partial_units_f(s_ctx.parcial_peso_tanda_g));

    s_ctx.parcial_pendiente = PARCIAL_PENDIENTE_NINGUNA;

    if (uds <= 0) {
        /* Menos de media unidad en el recipiente: no hay nada que sumar, y
         * registrar un 0 solo obligaria a vaciar para nada. */
        ESP_LOGW(TAG, "Tanda ignorada: %.2f g dan 0 unidades", s_ctx.parcial_peso_tanda_g);
        partial_refresh_ui();
        return;
    }

    if (es_nuevas) {
        s_ctx.parcial_nuevas += uds;
    } else {
        s_ctx.parcial_usadas += uds;
    }

    s_ctx.parcial_hay_ultima = true;
    s_ctx.parcial_ultima_uds = uds;
    s_ctx.parcial_ultima_era_nuevas = es_nuevas;
    s_ctx.parcial_ultima_peso_g = s_ctx.parcial_peso_tanda_g;

    /* Hasta que el recipiente se vacie, lo que marque la bascula sigue
     * siendo esta misma tanda: no se admite otra (ni se muestra), para no
     * contarla dos veces. */
    s_ctx.parcial_esperando_vaciado = true;
    s_ctx.parcial_tanda_lista = false;

    ESP_LOGI(TAG, "Tanda registrada: %d uds como %s (acumulado %d nuevas / %d usadas)",
             uds, es_nuevas ? "NUEVAS" : "USADAS", s_ctx.parcial_nuevas, s_ctx.parcial_usadas);

    partial_refresh_ui();
}

/* Boton NUEVAS/USADAS. Si el peso aun no esta estable no se descarta la
 * pulsacion: se recuerda y la tanda se registra sola en cuanto lo este. */
static void handle_partial_button(bool es_nuevas)
{
    if (s_state != APP_STATE_PARTIAL_COUNT) {
        return;
    }

    if (s_ctx.parcial_esperando_vaciado) {
        ESP_LOGD(TAG, "Clasificacion ignorada: falta vaciar el recipiente");
        return;
    }

    if (s_ctx.parcial_tanda_lista) {
        partial_register_batch(es_nuevas);
        return;
    }

    s_ctx.parcial_pendiente = es_nuevas ? PARCIAL_PENDIENTE_NUEVAS : PARCIAL_PENDIENTE_USADAS;
    partial_refresh_ui();
}

/* Deshacer la ultima tanda (un solo nivel): resta del contador al que se
 * sumo y deja el recipiente listo para volver a clasificarla, que es el
 * caso real - pulsar NUEVAS cuando era USADAS y darse cuenta al momento,
 * con la tanda todavia en la bascula. Si ya se habia vaciado, la siguiente
 * lectura estable sustituira ese peso sin mas. */
static void handle_partial_undo(void)
{
    if (!s_ctx.parcial_hay_ultima) {
        return;
    }

    if (s_ctx.parcial_ultima_era_nuevas) {
        s_ctx.parcial_nuevas -= s_ctx.parcial_ultima_uds;
    } else {
        s_ctx.parcial_usadas -= s_ctx.parcial_ultima_uds;
    }

    ESP_LOGI(TAG, "Deshecha la ultima tanda: %d uds de %s (acumulado %d nuevas / %d usadas)",
             s_ctx.parcial_ultima_uds, s_ctx.parcial_ultima_era_nuevas ? "NUEVAS" : "USADAS",
             s_ctx.parcial_nuevas, s_ctx.parcial_usadas);

    s_ctx.parcial_peso_tanda_g = s_ctx.parcial_ultima_peso_g;
    s_ctx.parcial_tanda_lista = true;
    s_ctx.parcial_esperando_vaciado = false;
    s_ctx.parcial_pendiente = PARCIAL_PENDIENTE_NINGUNA;
    s_ctx.parcial_hay_ultima = false;

    partial_refresh_ui();
}

/* "Finalizar": el total del articulo es la suma de las tandas contadas. */
static void finish_partial_counting(void)
{
    if (s_ctx.parcial_nuevas == 0 && s_ctx.parcial_usadas == 0) {
        /* Nada contado todavia: un toque accidental no debe dejar guardado
         * un 0/0 que luego habria que corregir volviendo a pasar el tag. */
        ESP_LOGD(TAG, "Finalizar ignorado: aun no se ha registrado ninguna tanda");
        return;
    }

    scale_cancel_weighing();
    ESP_LOGI(TAG, "Recuento por partes terminado: %d nuevas / %d usadas",
             s_ctx.parcial_nuevas, s_ctx.parcial_usadas);
    save_inventory_and_finish(s_ctx.parcial_nuevas, s_ctx.parcial_usadas);
}

static void handle_confirm_pressed(void)
{
    switch (s_state) {
    case APP_STATE_DUPLICATE_CONFIRM:
        s_ctx.overwrite_duplicate = true;
        s_state = APP_STATE_WAIT_WEIGHT_TOTAL;
        ui_show_description_and_wait_weight(s_ctx.descripcion, true);
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_REG_CODE_DUPLICATE: {
        /* "Sobrescribir": el codigo tecleado ya tenia OTRO tag vinculado
         * (tag fisico perdido/roto y sustituido) - se re-vincula a ESTE
         * tag, sin tocar descripcion/tara/peso_unitario ya guardados. */
        const master_item_t *existing = csv_master_find_by_codigo(s_ctx.codigo);
        if (!existing) {
            /* No debería pasar (desapareció justo entre medias): se
             * cancela sin más en vez de continuar con datos a medias. */
            ESP_LOGW(TAG, "Codigo %s ya no existe en datos maestros, se cancela", s_ctx.codigo);
            go_idle();
            break;
        }
        ESP_LOGI(TAG, "Re-vinculando codigo %s: tag anterior (%s) sustituido por %s",
                 s_ctx.codigo, existing->uid_nfc, s_ctx.reg_uid);
        link_tag_and_proceed(existing);
        break;
    }

    case APP_STATE_REG_CONFIRM_ARTICULO: {
        /* El operario ha confirmado que la descripcion mostrada es la del
         * material que tiene delante - ahora si, se decide Caso A/B. */
        const master_item_t *existing = csv_master_find_by_codigo(s_ctx.codigo);
        if (!existing) {
            ESP_LOGW(TAG, "Codigo %s ya no existe en datos maestros, se cancela", s_ctx.codigo);
            go_idle();
            break;
        }
        route_existing_articulo(existing);
        break;
    }

    case APP_STATE_WAIT_WEIGHT_USED:
        if (!s_ctx.tiene_lectura_usados) {
            break; /* aun no ha llegado ninguna lectura, se ignora el toque */
        }
        finish_used_weighing(s_ctx.pending_weight_g);
        break;

    case APP_STATE_PARTIAL_WAIT_TARE:
        if (!s_ctx.tiene_lectura_tara) {
            break; /* aun no ha llegado ninguna lectura, se ignora el toque */
        }
        s_ctx.parcial_tara_g = s_ctx.pending_weight_g;
        ESP_LOGI(TAG, "Pesada por partes: tara del recipiente = %.2f g", s_ctx.parcial_tara_g);
        s_state = APP_STATE_PARTIAL_COUNT;
        show_partial_count();
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_PARTIAL_COUNT: /* "Finalizar" */
        finish_partial_counting();
        break;

    case APP_STATE_REG_WAIT_TARE:
        if (!s_ctx.tiene_lectura_tara) {
            break; /* aun no ha llegado ninguna lectura, se ignora el toque */
        }
        s_ctx.tara_caja = s_ctx.pending_weight_g;
        ESP_LOGI(TAG, "Alta material nuevo: tara caja = %.2f g", s_ctx.tara_caja);
        if (s_ctx.unidades_totales > 0) {
            /* Unidades ya conocidas (tecleadas en REG_ENTER_UNITS): despejar
             * el peso_unitario, como siempre. */
            s_ctx.peso_unitario = (s_ctx.reg_peso_total - s_ctx.tara_caja) / s_ctx.unidades_totales;
            if (s_ctx.peso_unitario < 0.0f) {
                s_ctx.peso_unitario = 0.0f;
            }
        } else {
            /* Caso B, solo faltaba la tara: el peso_unitario ya se conocia
             * de antes (por eso no se paso por REG_ENTER_UNITS) y ahora que
             * ya se tiene la tara, las unidades salen solas del peso total -
             * mismo calculo que un articulo ya completo (units_from_weight
             * usa s_ctx.tara_caja/peso_unitario, ya puestos los dos). */
            s_ctx.unidades_totales = units_from_weight(s_ctx.reg_peso_total);
        }
        if (s_ctx.reg_is_update) {
            /* Actualizando: la descripcion (calibre/cabeza) no cambia, solo
             * tara/peso_unitario - no tiene sentido volver a pedirla. */
            finish_master_update();
        } else if (s_ctx.reg_completando_existente) {
            /* Caso B: la descripcion ya existia, tampoco se pide
             * calibre/cabeza - y a diferencia de reg_is_update, se sigue
             * inventariando en vez de volver a reposo. */
            finish_master_completion();
        } else {
            s_state = APP_STATE_REG_ENTER_CALIBRE;
            ui_show_keypad("Calibre (mm)", true, 6, NULL);
        }
        break;

    case APP_STATE_CONFIRM_WEIGHT:
        switch (s_ctx.pending_weight_state) {
        case APP_STATE_REG_WAIT_WEIGHT_TOTAL:
            s_ctx.reg_peso_total = s_ctx.pending_weight_g;
            if (s_ctx.reg_is_update) {
                /* Actualizando un codigo ya existente: no hay que pedirlo,
                 * ya se conoce (s_ctx.codigo no se ha tocado). */
                ESP_LOGI(TAG, "Actualizar datos maestros %s: peso total = %.2f g",
                         s_ctx.codigo, s_ctx.pending_weight_g);
                s_state = APP_STATE_REG_ENTER_UNITS;
                ui_show_keypad("Unidades totales en la caja", false, 5, NULL);
            } else {
                ESP_LOGI(TAG, "Alta material nuevo: peso total = %.2f g", s_ctx.pending_weight_g);
                s_state = APP_STATE_REG_ENTER_CODE;
                ui_show_keypad("Ultimos digitos del codigo (13 + estos digitos)", false, REG_CODIGO_MAX_DIGITS, NULL);
            }
            break;

        default:
            break;
        }
        break;

    case APP_STATE_SETTINGS_EXPLAIN: {
        /* "Editar": abre el teclado con el valor actual precargado, para
         * corregirlo en vez de tener que teclearlo entero de nuevo. */
        const settings_def_t *def = settings_get_def(s_ctx.settings_selected);
        char valor_inicial[24];
        if (def->decimals > 0) {
            snprintf(valor_inicial, sizeof(valor_inicial), "%.1f", (double)settings_get(def->id));
        } else {
            snprintf(valor_inicial, sizeof(valor_inicial), "%.0f", (double)settings_get(def->id));
        }
        s_state = APP_STATE_SETTINGS_EDIT;
        ui_show_keypad(def->name, def->decimals > 0, 6, valor_inicial);
        break;
    }

    case APP_STATE_SETTINGS_RESET_CONFIRM:
        settings_reset_defaults();
        s_state = APP_STATE_SETTINGS_LIST;
        ui_show_settings_list();
        break;

    default:
        break;
    }
}

static void handle_keypad_confirmed(const char *text)
{
    switch (s_state) {
    case APP_STATE_REG_ENTER_CODE: {
        long digits = strtol(text, NULL, 10);
        if (digits < 0) {
            digits = 0;
        }
        long codigo_num = REG_CODIGO_BASE + digits;
        snprintf(s_ctx.codigo, sizeof(s_ctx.codigo), "%ld", codigo_num);

        const master_item_t *existing = csv_master_find_by_codigo(s_ctx.codigo);

        if (existing && existing->uid_nfc[0] != '\0') {
            /* Ya hay OTRO tag vinculado a este codigo. Puede ser un error
             * de tecleo, pero tambien el caso real de "se perdio/rompio el
             * tag fisico de esta caja y se le ha pegado uno nuevo" - se
             * deja elegir en vez de bloquear sin mas: Sobrescribir
             * re-vincula este tag (ver APP_STATE_REG_CODE_DUPLICATE en
             * handle_confirm_pressed), Cancelar vuelve a reposo sin tocar
             * nada (ya cableado, ver handle_cancel_pressed). */
            ESP_LOGW(TAG, "Codigo %s ya vinculado a otro tag (%s)", s_ctx.codigo, existing->uid_nfc);
            s_state = APP_STATE_REG_CODE_DUPLICATE;
            ui_show_tag_relink_warning(s_ctx.codigo, existing->descripcion);
            break;
        }

        if (existing) {
            /* Material precatalogado (fila en datos_maestros.csv sin tag
             * aun vinculado): antes de tocar nada, se muestra la
             * descripcion a toda pantalla para que el operario valide que
             * el codigo tecleado es de verdad el material que tiene
             * delante (evita arrastrar un error de tecleo o una caja mal
             * etiquetada varios pasos sin darse cuenta). La decision de
             * Caso A/B (ver route_existing_articulo()) se toma DESPUES de
             * confirmar, no aqui. */
            strlcpy(s_ctx.descripcion, existing->descripcion, sizeof(s_ctx.descripcion));
            s_ctx.tara_caja = existing->tara_caja;
            s_ctx.peso_unitario = existing->peso_unitario;

            s_state = APP_STATE_REG_CONFIRM_ARTICULO;
            ui_show_confirm_articulo(s_ctx.codigo, s_ctx.descripcion);
            break;
        }

        /* Codigo totalmente nuevo: sigue el alta completa (unidades, tara,
         * calibre, cabeza). */
        s_state = APP_STATE_REG_ENTER_UNITS;
        ui_show_keypad("Unidades totales en la caja", false, 5, NULL);
        break;
    }

    case APP_STATE_REG_ENTER_UNITS: {
        int unidades = (int)strtol(text, NULL, 10);
        if (unidades <= 0) {
            /* invalido, se vuelve a pedir sin perder el resto del contexto */
            ui_show_keypad("Unidades totales (numero mayor que 0)", false, 5, NULL);
            break;
        }
        s_ctx.unidades_totales = unidades;

        if (s_ctx.reg_completando_existente && s_ctx.tara_caja > 0.0f) {
            /* Caso B, solo faltaba el peso_unitario: la tara ya se conocia
             * de antes, no hace falta volver a pesar - se calcula ya
             * mismo y se termina. */
            s_ctx.peso_unitario = (s_ctx.reg_peso_total - s_ctx.tara_caja) / s_ctx.unidades_totales;
            if (s_ctx.peso_unitario < 0.0f) {
                s_ctx.peso_unitario = 0.0f;
            }
            finish_master_completion();
            break;
        }

        s_state = APP_STATE_REG_WAIT_TARE;
        s_ctx.tiene_lectura_tara = false;
        ui_show_wait_tare();
        start_weighing_for_state(s_state);
        break;
    }

    case APP_STATE_REG_ENTER_CALIBRE: {
        s_ctx.reg_calibre = strtof(text, NULL);
        s_state = APP_STATE_REG_ENTER_CABEZA;
        ui_show_keypad("Cabeza (mm)", true, 6, NULL);
        break;
    }

    case APP_STATE_REG_ENTER_CABEZA: {
        /* Solo se llega aqui dando de alta un material TOTALMENTE nuevo (ni
         * codigo ni descripcion existian): al completar uno precatalogado
         * (reg_completando_existente) o actualizar uno existente
         * (reg_is_update) ya se termina antes, justo tras la tara, sin
         * tocar la descripcion.
         *
         * La descripcion se marca "TEMPORAL calibre x cabeza" en vez de
         * generar la frase completa del bulon: no todos los bulones siguen
         * el mismo plano, y el calibre/cabeza reales son mejor pista que
         * una descripcion inventada. Luego, en Excel, se buscan las filas
         * con "TEMPORAL" y se les pone la descripcion correcta a mano. */
        float cabeza = strtof(text, NULL);
        snprintf(s_ctx.descripcion, sizeof(s_ctx.descripcion),
                 "TEMPORAL %.2fx%.2f", s_ctx.reg_calibre, cabeza);
        ESP_LOGI(TAG, "Alta material nuevo: codigo=%s descripcion=%s tara=%.2fg peso_unit=%.4fg",
                 s_ctx.codigo, s_ctx.descripcion, s_ctx.tara_caja, s_ctx.peso_unitario);
        finish_new_material_registration();
        break;
    }

    case APP_STATE_SETTINGS_EDIT: {
        const settings_def_t *def = settings_get_def(s_ctx.settings_selected);
        float value = strtof(text, NULL);
        if (settings_set(s_ctx.settings_selected, value) != ESP_OK) {
            /* Fuera de rango: se vuelve a pedir sin perder el ajuste
             * seleccionado, con el rango en el titulo para no fallar dos
             * veces seguidas por lo mismo. */
            char titulo[96];
            if (def->decimals > 0) {
                snprintf(titulo, sizeof(titulo), "%s (%.1f - %.1f)",
                         def->name, (double)def->min_value, (double)def->max_value);
            } else {
                snprintf(titulo, sizeof(titulo), "%s (%.0f - %.0f)",
                         def->name, (double)def->min_value, (double)def->max_value);
            }
            ui_show_keypad(titulo, def->decimals > 0, 6, text);
            break;
        }
        s_state = APP_STATE_SETTINGS_LIST;
        ui_show_settings_list();
        break;
    }

    default:
        break;
    }
}

static void handle_retry_pressed(void)
{
    switch (s_state) {
    case APP_STATE_ERROR_DISMISSABLE:
    case APP_STATE_DUPLICATE_CONFIRM:
        go_idle();
        break;

    case APP_STATE_REG_CONFIRM_ARTICULO:
        /* "Revisar codigo": la descripcion mostrada NO es el material que
         * se esta contando - probable error de tecleo. Se vuelve a pedir
         * el codigo sin perder el resto del contexto (peso total ya
         * pesado, tag ya leido). */
        s_state = APP_STATE_REG_ENTER_CODE;
        ui_show_keypad("Ultimos digitos del codigo (13 + estos digitos)", false, REG_CODIGO_MAX_DIGITS, NULL);
        break;

    case APP_STATE_WAIT_WEIGHT_USED:
        /* La pesada de la caja completa no fue correcta: se repite desde
         * cero, reutilizando descripcion/tara/peso_unitario ya conocidos. */
        s_state = APP_STATE_WAIT_WEIGHT_TOTAL;
        ui_show_description_and_wait_weight(s_ctx.descripcion, true);
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_WAIT_WEIGHT_TOTAL:
        /* "Pesar por partes": la caja no cabe/pesa mas de lo que admite la
         * bascula, se cuenta por tandas. */
        handle_partial_mode_pressed();
        break;

    case APP_STATE_PARTIAL_COUNT: /* "Deshacer" */
        handle_partial_undo();
        break;

    case APP_STATE_TIMEOUT_WEIGHT:
        s_state = s_ctx.pending_weight_state;
        switch (s_state) {
        case APP_STATE_WAIT_WEIGHT_TOTAL:
            ui_show_description_and_wait_weight(s_ctx.descripcion, true);
            break;
        case APP_STATE_REG_WAIT_WEIGHT_TOTAL:
            ui_show_description_and_wait_weight(s_ctx.reg_is_update ? MSG_UPDATE_WEIGH_TOTAL : MSG_REG_WEIGH_TOTAL, false);
            break;
        default:
            break;
        }
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_CONFIRM_WEIGHT:
        /* "Repetir pesada": se vuelve al mismo paso de pesada sin perder
         * el resto del contexto ya recopilado. */
        s_state = s_ctx.pending_weight_state;
        switch (s_state) {
        case APP_STATE_REG_WAIT_WEIGHT_TOTAL:
            ui_show_description_and_wait_weight(s_ctx.reg_is_update ? MSG_UPDATE_WEIGH_TOTAL : MSG_REG_WEIGH_TOTAL, false);
            break;
        default:
            break;
        }
        start_weighing_for_state(s_state);
        break;

    case APP_STATE_INCONSISTENT_WEIGHT:
        s_state = APP_STATE_WAIT_WEIGHT_USED;
        s_ctx.tiene_lectura_usados = false;
        show_wait_weight_used();
        start_weighing_for_state(s_state);
        break;

    default:
        break;
    }
}

/* Activa el modo USB Mass Storage bajo demanda (solo desde reposo).
 * Reinicia el dispositivo para arrancar DIRECTO en modo USB, sin levantar
 * NFC/bascula/FSM - ver el comentario de usb_msc_request_boot_and_restart()
 * para el porque. Esta funcion no vuelve. */
static void handle_usb_mode_pressed(void)
{
    if (s_state != APP_STATE_IDLE) {
        return;
    }

    usb_msc_request_boot_and_restart();
}

/* Boton "Ajustes" en reposo (solo desde ahi: no tiene sentido entrar en
 * mitad de una pesada o un alta en curso). */
static void handle_settings_pressed(void)
{
    if (s_state != APP_STATE_IDLE) {
        return;
    }
    s_state = APP_STATE_SETTINGS_LIST;
    ui_show_settings_list();
}

/* Fila de la lista de ajustes tocada: guarda cual es y muestra su
 * descripcion/valor actual/rango antes de dejar editarlo. */
static void handle_settings_row_pressed(int setting_id)
{
    if (s_state != APP_STATE_SETTINGS_LIST) {
        return;
    }
    if (setting_id < 0 || setting_id >= (int)SETTING_COUNT) {
        return;
    }
    s_ctx.settings_selected = (setting_id_t)setting_id;
    const settings_def_t *def = settings_get_def(s_ctx.settings_selected);

    char valor_buf[24];
    char rango_buf[40];
    if (def->decimals > 0) {
        snprintf(valor_buf, sizeof(valor_buf), "%.1f %s", (double)settings_get(def->id), def->unit);
        snprintf(rango_buf, sizeof(rango_buf), "%.1f - %.1f %s",
                 (double)def->min_value, (double)def->max_value, def->unit);
    } else {
        snprintf(valor_buf, sizeof(valor_buf), "%.0f %s", (double)settings_get(def->id), def->unit);
        snprintf(rango_buf, sizeof(rango_buf), "%.0f - %.0f %s",
                 (double)def->min_value, (double)def->max_value, def->unit);
    }

    s_state = APP_STATE_SETTINGS_EXPLAIN;
    ui_show_settings_explain(def->name, def->description, valor_buf, rango_buf);
}

/* "Restablecer valores de fabrica" en la lista: pide confirmar antes de
 * tocar los 8 ajustes a la vez. */
static void handle_settings_reset_pressed(void)
{
    if (s_state != APP_STATE_SETTINGS_LIST) {
        return;
    }
    s_state = APP_STATE_SETTINGS_RESET_CONFIRM;
    ui_show_settings_reset_confirm();
}

static void handle_cancel_pressed(void)
{
    switch (s_state) {
    case APP_STATE_WAIT_WEIGHT_TOTAL:
    case APP_STATE_WAIT_WEIGHT_USED:
    case APP_STATE_TIMEOUT_WEIGHT:
    case APP_STATE_INCONSISTENT_WEIGHT:
    case APP_STATE_CONFIRM_WEIGHT:
    case APP_STATE_REG_WAIT_WEIGHT_TOTAL:
    case APP_STATE_REG_ENTER_CODE:
    case APP_STATE_REG_CODE_DUPLICATE:
    case APP_STATE_REG_CONFIRM_ARTICULO:
    case APP_STATE_REG_ENTER_UNITS:
    case APP_STATE_REG_WAIT_TARE:
    case APP_STATE_REG_ENTER_CALIBRE:
    case APP_STATE_REG_ENTER_CABEZA:
    case APP_STATE_PARTIAL_WAIT_TARE:
    case APP_STATE_PARTIAL_COUNT:
    case APP_STATE_SETTINGS_LIST:
        go_idle();
        break;
    case APP_STATE_SETTINGS_EXPLAIN:
    case APP_STATE_SETTINGS_EDIT:
    case APP_STATE_SETTINGS_RESET_CONFIRM:
        /* "Volver"/Cancelar dentro del menu de ajustes no sale a reposo,
         * vuelve un paso atras dentro del propio menu. */
        s_state = APP_STATE_SETTINGS_LIST;
        ui_show_settings_list();
        break;
    default:
        break;
    }
}

static void app_task(void *arg)
{
    app_event_t evt;
    while (1) {
        /* Timeout corto (en vez de portMAX_DELAY) para poder comprobar la
         * inactividad de pantalla aunque no llegue ningun evento. */
        BaseType_t got_evt = xQueueReceive(g_app_event_queue, &evt, pdMS_TO_TICKS(SCREEN_DIM_CHECK_MS));
        check_screen_dim();
        if (got_evt != pdTRUE) {
            continue;
        }
        switch (evt.type) {
        case APP_EVT_NFC_TAG:
            handle_nfc_tag(evt.data.nfc_tag.uid_hex);
            break;
        case APP_EVT_SCALE_STABLE:
            handle_scale_stable(evt.data.scale_stable.weight_g);
            break;
        case APP_EVT_SCALE_READING:
            handle_scale_reading(evt.data.scale_reading.weight_g, evt.data.scale_reading.stable);
            break;
        case APP_EVT_SCALE_TIMEOUT:
            handle_scale_timeout();
            break;
        case APP_EVT_UI_CONFIRM_PRESSED:
            handle_confirm_pressed();
            break;
        case APP_EVT_UI_CANCEL_PRESSED:
            handle_cancel_pressed();
            break;
        case APP_EVT_UI_RETRY_PRESSED:
            handle_retry_pressed();
            break;
        case APP_EVT_UI_KEYPAD_CONFIRMED:
            handle_keypad_confirmed(evt.data.keypad.text);
            break;
        case APP_EVT_UI_USB_MODE_PRESSED:
            handle_usb_mode_pressed();
            break;
        case APP_EVT_UI_UPDATE_MASTER_PRESSED:
            handle_update_master_pressed();
            break;
        case APP_EVT_UI_SETTINGS_PRESSED:
            handle_settings_pressed();
            break;
        case APP_EVT_UI_SETTINGS_ROW_PRESSED:
            handle_settings_row_pressed(evt.data.settings_row.setting_id);
            break;
        case APP_EVT_UI_SETTINGS_RESET_PRESSED:
            handle_settings_reset_pressed();
            break;
        case APP_EVT_UI_PARTIAL_NUEVAS_PRESSED:
            handle_partial_button(true);
            break;
        case APP_EVT_UI_PARTIAL_USADAS_PRESSED:
            handle_partial_button(false);
            break;
        }
    }
}

void app_fsm_start(void)
{
    go_idle(); /* refresca la pantalla de reposo con el historial ya cargado de la SD */
    xTaskCreate(app_task, "app_fsm", 4096, NULL, 5, NULL);
}
