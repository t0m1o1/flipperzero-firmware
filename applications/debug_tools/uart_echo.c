#include <furi.h>
#include <m-string.h>
#include <gui/gui.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <gui/elements.h>
#include <stream_buffer.h>
#include <furi_hal_uart.h>
#include <furi_hal_console.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/dialog_ex.h>

#define LINES_ON_SCREEN 6
#define COLUMNS_ON_SCREEN 21

typedef struct UartDumpModel UartDumpModel;

typedef struct {
    Gui* gui;
    NotificationApp* notification;
    ViewDispatcher* view_dispatcher;
    View* view;
    FuriThread* worker_thread;
    StreamBufferHandle_t rx_stream;
} UartEchoApp;

typedef struct {
    string_t text;
} ListElement;

struct UartDumpModel {
    ListElement* list[LINES_ON_SCREEN];
    uint8_t line;

    char last_char;
    bool escape;
};

typedef enum {
    WorkerEventReserved = (1 << 0), // Reserved for StreamBuffer internal event
    WorkerEventStop = (1 << 1),
    WorkerEventRx = (1 << 2),
} WorkerEventFlags;

#define WORKER_EVENTS_MASK (WorkerEventStop | WorkerEventRx)

const NotificationSequence sequence_notification = {
    &message_display_backlight_on,
    &message_green_255,
    &message_delay_10,
    NULL,
};

static void uart_echo_view_draw_callback(Canvas* canvas, void* _model) {
    UartDumpModel* model = _model;

    // Prepare canvas
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);

    for(size_t i = 0; i < LINES_ON_SCREEN; i++) {
        canvas_draw_str(
            canvas,
            0,
            (i + 1) * (canvas_current_font_height(canvas) - 1),
            string_get_cstr(model->list[i]->text));

        if(i == model->line) {
            uint8_t width = canvas_string_width(canvas, string_get_cstr(model->list[i]->text));

            canvas_draw_box(
                canvas,
                width,
                (i) * (canvas_current_font_height(canvas) - 1) + 2,
                2,
                canvas_current_font_height(canvas) - 2);
        }
    }
}

static bool uart_echo_view_input_callback(InputEvent* event, void* context) {
    UNUSED(event);
    UNUSED(context);
    return false;
}

static uint32_t uart_echo_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

static void uart_echo_on_irq_cb(UartIrqEvent ev, uint8_t data, void* context) {
    furi_assert(context);
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    UartEchoApp* app = context;

    if(ev == UartIrqEventRXNE) {
        xStreamBufferSendFromISR(app->rx_stream, &data, 1, &xHigherPriorityTaskWoken);
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventRx);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

static void uart_echo_push_to_list(UartDumpModel* model, const char data) {
    if(model->escape) {
        // escape code end with letter
        if((data >= 'a' && data <= 'z') || (data >= 'A' && data <= 'Z')) {
            model->escape = false;
        }
    } else if(data == '[' && model->last_char == '\e') {
        // "Esc[" is a escape code
        model->escape = true;
    } else if((data >= ' ' && data <= '~') || (data == '\n' || data == '\r')) {
        bool new_string_needed = false;
        if(string_size(model->list[model->line]->text) >= COLUMNS_ON_SCREEN) {
            new_string_needed = true;
        } else if((data == '\n' || data == '\r')) {
            // pack line breaks
            if(model->last_char != '\n' && model->last_char != '\r') {
                new_string_needed = true;
            }
        }

        if(new_string_needed) {
            if((model->line + 1) < LINES_ON_SCREEN) {
                model->line += 1;
            } else {
                ListElement* first = model->list[0];

                for(size_t i = 1; i < LINES_ON_SCREEN; i++) {
                    model->list[i - 1] = model->list[i];
                }

                string_reset(first->text);
                model->list[model->line] = first;
            }
        }

        if(data != '\n' && data != '\r') {
            string_push_back(model->list[model->line]->text, data);
        }
    }
    model->last_char = data;
}

static int32_t uart_echo_worker(void* context) {
    furi_assert(context);
    UartEchoApp* app = context;

    while(1) {
        uint32_t events =
            furi_thread_flags_wait(WORKER_EVENTS_MASK, osFlagsWaitAny, osWaitForever);
        furi_check((events & osFlagsError) == 0);

        if(events & WorkerEventStop) break;
        if(events & WorkerEventRx) {
            size_t length = 0;
            do {
                uint8_t data[64];
                length = xStreamBufferReceive(app->rx_stream, data, 64, 0);
                if(length > 0) {
                    furi_hal_uart_tx(FuriHalUartIdUSART1, data, length);
                    with_view_model(
                        app->view, (UartDumpModel * model) {
                            for(size_t i = 0; i < length; i++) {
                                uart_echo_push_to_list(model, data[i]);
                            }
                            return false;
                        });
                }
            } while(length > 0);

            notification_message(app->notification, &sequence_notification);
            with_view_model(
                app->view, (UartDumpModel * model) {
                    UNUSED(model);
                    return true;
                });
        }
    }

    return 0;
}

static UartEchoApp* uart_echo_app_alloc() {
    UartEchoApp* app = malloc(sizeof(UartEchoApp));

    app->rx_stream = xStreamBufferCreate(2048, 1);

    // Gui
    app->gui = furi_record_open("gui");
    app->notification = furi_record_open("notification");

    // View dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    // Views
    app->view = view_alloc();
    view_set_draw_callback(app->view, uart_echo_view_draw_callback);
    view_set_input_callback(app->view, uart_echo_view_input_callback);
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(UartDumpModel));
    with_view_model(
        app->view, (UartDumpModel * model) {
            for(size_t i = 0; i < LINES_ON_SCREEN; i++) {
                model->line = 0;
                model->escape = false;
                model->list[i] = malloc(sizeof(ListElement));
                string_init(model->list[i]->text);
            }
            return true;
        });

    view_set_previous_callback(app->view, uart_echo_exit);
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);

    // Enable uart listener
    furi_hal_console_disable();
    furi_hal_uart_set_br(FuriHalUartIdUSART1, 115200);
    furi_hal_uart_set_irq_cb(FuriHalUartIdUSART1, uart_echo_on_irq_cb, app);

    app->worker_thread = furi_thread_alloc();
    furi_thread_set_name(app->worker_thread, "UsbUartWorker");
    furi_thread_set_stack_size(app->worker_thread, 1024);
    furi_thread_set_context(app->worker_thread, app);
    furi_thread_set_callback(app->worker_thread, uart_echo_worker);
    furi_thread_start(app->worker_thread);

    return app;
}

static void uart_echo_app_free(UartEchoApp* app) {
    furi_assert(app);

    furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventStop);
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    furi_hal_console_enable();

    // Free views
    view_dispatcher_remove_view(app->view_dispatcher, 0);

    with_view_model(
        app->view, (UartDumpModel * model) {
            for(size_t i = 0; i < LINES_ON_SCREEN; i++) {
                string_clear(model->list[i]->text);
                free(model->list[i]);
            }
            return true;
        });
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    // Close gui record
    furi_record_close("gui");
    furi_record_close("notification");
    app->gui = NULL;

    vStreamBufferDelete(app->rx_stream);

    // Free rest
    free(app);
}

int32_t uart_echo_app(void* p) {
    UNUSED(p);
    UartEchoApp* app = uart_echo_app_alloc();
    view_dispatcher_run(app->view_dispatcher);
    uart_echo_app_free(app);
    return 0;
}
