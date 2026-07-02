// Import global project config
#include "config.h"

hid_keyboard_report_t last_keyboard_report[HISTORY_SIZE];
hid_mouse_report_t last_mouse_report;
static uint8_t last_physical_mouse_buttons;

#define AUTO_CLICK_BUTTON_COUNT 2
#define AUTO_CLICK_INTERVAL_US 62500
#define AUTO_CLICK_HALF_INTERVAL_US (AUTO_CLICK_INTERVAL_US / 2)
#define AUTO_CLICK_RECENT_WINDOW_US AUTO_CLICK_INTERVAL_US

typedef struct {
    uint8_t button_mask;
    bool active;
    bool physical_down;
    bool generated_pressed;
    bool timer_running;
    int64_t recent_until;
    esp_timer_handle_t timer;
    esp_timer_create_args_t timer_args;
} mouse_auto_click_t;

static mouse_auto_click_t mouse_auto_clickers[AUTO_CLICK_BUTTON_COUNT] = {
    {.button_mask = MOUSE_BUTTON_LEFT},
    {.button_mask = MOUSE_BUTTON_RIGHT},
};

group_sequence_t group_sequence;

static void mouse_auto_click_callback(void* arg);
static void start_mouse_auto_click(mouse_auto_click_t* clicker, int64_t now);
static void stop_mouse_auto_click(mouse_auto_click_t* clicker);
static void schedule_mouse_auto_click(mouse_auto_click_t* clicker, uint64_t delay_us);

void macro_init(){
    for (int i = 0; i < AUTO_CLICK_BUTTON_COUNT; i++){
        mouse_auto_click_t* clicker = &mouse_auto_clickers[i];
        clicker->timer_args.callback = &mouse_auto_click_callback;
        clicker->timer_args.arg = clicker;
        clicker->timer_args.name = clicker->button_mask == MOUSE_BUTTON_LEFT ? "mouse_left_click" : "mouse_right_click";
        esp_timer_create(&clicker->timer_args, &clicker->timer);
    }

    // Copy sequence
    group_sequence = macro_sequence;
    // For each macro sequence define by the user
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++){
        key_modification_sequence_t* sequence = &group_sequence.list[i];
        // Ignore empty sequence
        if (sequence->size == 0) continue;
        // Set callback
        sequence->timer_args.callback = &macro_sequence_callback;
        sequence->timer_args.arg = &group_sequence.list[i];
        sequence->timer_args.name = malloc(15*sizeof(char));
        snprintf((char*)sequence->timer_args.name, 15, "sequence%d", i);

        // Instanciate an ESP timer
        esp_timer_create(&sequence->timer_args, &sequence->timer);
    }
}

// Prehook for macro HID transmission. Return true to end transmission chain. Default to false.
bool macro_prehook_transmission(hid_transmit_t* report){
    
    // --- START USER CUSTOM MACRO
    if (report->header == HEADER_HID_KEYBOARD){
        if (keycode_contains_key(report->event.keyboard, HID_KEY_A) && keycode_contains_key(report->event.keyboard, HID_KEY_D)){
            if (keycode_contains_key(last_keyboard_report[0], HID_KEY_A)){
                remove_keycode(&report->event.keyboard, HID_KEY_A);
            } else if (keycode_contains_key(last_keyboard_report[0], HID_KEY_D)){
                remove_keycode(&report->event.keyboard, HID_KEY_D);
            }
        }
    }
    // --- END
     
    // Example: Remove a KEY
    // remove_keycode(report, HID_KEY_C);

    // Exemple: Skip all A key
    // if (keycode_contains_key(report->event.keyboard, HID_KEY_A) return true;

    return false;
}

void macro_posthook_transmission(hid_transmit_t* report){

    #if DEBUG_LOG
    ESP_LOGI(pcTaskGetName(NULL), "posthook(): Start function");
    if (report->header == HEADER_HID_KEYBOARD) {
        print_keyboard_report(pcTaskGetName(NULL), report->event.keyboard);
    } else if (report->header == HEADER_HID_MOUSE) {
        print_mouse_report(pcTaskGetName(NULL), report->event.mouse);
    }
    #endif

    // Case n°1: Keyboard HID
    if (report->header == HEADER_HID_KEYBOARD){
        // Update last report transmission, like this all macro are added to real keys press by user
        last_keyboard_report[1] = last_keyboard_report[0];
        last_keyboard_report[0] = report->event.keyboard;

        // --- START USER CUSTOM MACRO
        // --- END

        // Manage sequence start:
        for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++){
            key_modification_sequence_t* sequence = &group_sequence.list[i];
            // Ignore empty sequence
            if (sequence->size == 0) break;
            // If a press key is defined for sequence
            if (sequence->event_press.header == HEADER_HID_KEYBOARD && keyboard_report_contains_event(last_keyboard_report[0], sequence->event_press.event.keyboard)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting keyboard press macro: %s", sequence->timer_args.name);
                #endif
                reset_sequence(sequence);
                start_sequence(sequence);
            }
            // If a unpress key is defined for sequence
            if (sequence->event_release.header == HEADER_HID_KEYBOARD && keyboard_report_contains_event(last_keyboard_report[1], sequence->event_release.event.keyboard) && !keyboard_report_contains_event(last_keyboard_report[0], sequence->event_release.event.keyboard)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting keyboard release macro: %s", sequence->timer_args.name);
                #endif
                reset_sequence(sequence);
                start_sequence(sequence);
            }
            // If a recording sequence
            if (sequence->save_press.header == HEADER_HID_KEYBOARD && sequence->is_recording){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Recording keyboard event");
                #endif
                add_keyboard_record(sequence, *report);
            }
            // If a save press key is defined for sequence
            if (sequence->save_press.header == HEADER_HID_KEYBOARD && keyboard_report_contains_event(last_keyboard_report[0], sequence->save_press.event.keyboard)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting keyboard save macro: %s", sequence->timer_args.name);
                #endif
                reset_sequence(sequence);
                sequence->is_recording = true;
            }
        }
    // Case n°2: Mouse HID
    } else if (report->header == HEADER_HID_MOUSE){
        // Update last report transmission, like this all macro are added to real keys press by user
        last_mouse_report = report->event.mouse;

        // --- START USER CUSTOM MACRO
        uint8_t previous_buttons = last_physical_mouse_buttons;
        uint8_t current_buttons = report->event.mouse.buttons;
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < AUTO_CLICK_BUTTON_COUNT; i++){
            mouse_auto_click_t* clicker = &mouse_auto_clickers[i];
            bool was_down = (previous_buttons & clicker->button_mask) != 0;
            bool is_down = (current_buttons & clicker->button_mask) != 0;
            if (!was_down && is_down){
                start_mouse_auto_click(clicker, now);
            } else if (was_down && !is_down){
                clicker->physical_down = false;
                clicker->recent_until = now + AUTO_CLICK_RECENT_WINDOW_US;
            }
        }
        last_physical_mouse_buttons = current_buttons;
        // --- END

        // Manage sequence start:
        for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++){
            key_modification_sequence_t* sequence = &group_sequence.list[i];
            // Ignore empty sequence
            if (sequence->size == 0) break;
            // If a press key is defined for sequence
            if (sequence->event_press.header == HEADER_HID_MOUSE && mouse_report_contains_event(last_mouse_report, sequence->event_press.event.mouse)){
                #if DEBUG_LOG
                ESP_LOGI(pcTaskGetName(NULL), "posthook(): Starting mouse press macro: %s", sequence->timer_args.name);
                #endif
                reset_sequence(sequence);
                start_sequence(sequence);
            }
        }
    }
}

static void schedule_mouse_auto_click(mouse_auto_click_t* clicker, uint64_t delay_us) {
    if (clicker->timer_running) return;
    clicker->timer_running = true;
    esp_timer_start_once(clicker->timer, delay_us);
}

static void start_mouse_auto_click(mouse_auto_click_t* clicker, int64_t now) {
    clicker->active = true;
    clicker->physical_down = true;
    clicker->generated_pressed = true;
    clicker->recent_until = now + AUTO_CLICK_RECENT_WINDOW_US;
    schedule_mouse_auto_click(clicker, AUTO_CLICK_HALF_INTERVAL_US);
}

static void stop_mouse_auto_click(mouse_auto_click_t* clicker) {
    if (clicker->timer_running) {
        esp_timer_stop(clicker->timer);
        clicker->timer_running = false;
    }
    clicker->active = false;
    clicker->physical_down = false;
    clicker->generated_pressed = false;
    clicker->recent_until = 0;
}

static void mouse_auto_click_callback(void* arg) {
    mouse_auto_click_t* clicker = (mouse_auto_click_t*) arg;
    clicker->timer_running = false;

    int64_t now = esp_timer_get_time();
    if (!clicker->physical_down && now >= clicker->recent_until) {
        stop_mouse_auto_click(clicker);
        return;
    }

    clicker->generated_pressed = !clicker->generated_pressed;

    hid_transmit_t click_report;
    click_report.header = HEADER_HID_MOUSE;
    click_report.event.mouse = last_mouse_report;
    click_report.event.mouse.buttons &= ~clicker->button_mask;
    if (clicker->generated_pressed) {
        click_report.event.mouse.buttons |= clicker->button_mask;
    }
    click_report.event.mouse.x = 0;
    click_report.event.mouse.y = 0;
    click_report.event.mouse.wheel = 0;
    click_report.event.mouse.pan = 0;

    hid_add_report(click_report);
    schedule_mouse_auto_click(clicker, AUTO_CLICK_HALF_INTERVAL_US);
}


void macro_sequence_callback(void* arg) {
    // Get the key sequence from arguments
    key_modification_sequence_t* key_seq = (key_modification_sequence_t*) arg;
    hid_transmit_t macro_event = key_seq->list[key_seq->pos].event;
    // Copy last humain HID report...
    hid_transmit_t copy_report;
    if (macro_event.header == HEADER_HID_KEYBOARD){
        copy_report.header = HEADER_HID_KEYBOARD;
        copy_report.event.keyboard = last_keyboard_report[0];
    } else if (macro_event.header == HEADER_HID_MOUSE) {
        copy_report.header = HEADER_HID_MOUSE;
        copy_report.event.mouse = last_mouse_report;
        copy_report.event.mouse.x = 0; copy_report.event.mouse.y = 0; copy_report.event.mouse.wheel = 0;
    } else {
        #if DEBUG_LOG
        ESP_LOGI(pcTaskGetName(NULL), "macro_sequence(): Invalid macro sequence n°%d (header: %d): %s", key_seq->pos, macro_event.header, key_seq->timer_args.name);
        #endif
        return;
    }

    // ... and add the custom key to the sequence previous key.
    key_seq->previous_key = macro_event;
    // Previous key of all sequence are added to copy report to send: all sequences can run in parallel
    for (int i = 0; i < MAX_KEY_MODIFICATION_SEQUENCE; i++){
        key_modification_sequence_t* sequence = &group_sequence.list[i];
        // Ignore empty sequence
        if (sequence->size == 0) continue;
        // Skip sequence with not same HID type
        if (sequence->previous_key.header != macro_event.header) continue;
        // Add the keycode to report
        add_event_to_report(&copy_report, sequence->previous_key);
    }
    // In case of mouse, add mouvement to report
    if (macro_event.header == HEADER_HID_MOUSE) set_mouse_movement_to_report(&copy_report.event.mouse, macro_event.event.mouse);

    // Send the modified report to the HID task for USB transmission
    #if DEBUG_LOG
    ESP_LOGI(pcTaskGetName(NULL), "macro_sequence(): Send report from: %s", key_seq->timer_args.name);
    #endif
    hid_add_report(copy_report);

    // Schedule next sequence key with esp_timer
    // Increase the sequence position
    key_seq->pos++;
    // if end of sequence, reset
    if (key_seq->pos >= key_seq->size){
        reset_sequence(key_seq);
        if (key_seq->loop){
            // Ignore end of sequence, try to continue
        } else {
            return;
        }
    }
    // ... else restart timer with next duration
    // (but in a loop the press key must still be pressed)
    if (key_seq->loop && (
         (key_seq->event_press.header == HEADER_HID_KEYBOARD && !keyboard_report_contains_event(last_keyboard_report[0], key_seq->event_press.event.keyboard)) ||
         (key_seq->event_press.header == HEADER_HID_MOUSE && !mouse_report_contains_event(last_mouse_report, key_seq->event_press.event.mouse))
        )
       ){
        reset_sequence(key_seq);
        return;
    }  
    start_sequence(key_seq);
    return;
}
