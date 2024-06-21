// Include MicroPython API.
#include <stdio.h>
#include <string.h>
#include "py/runtime.h"
#include "machine_pin.h"
#include "hardware/regs/intctrl.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"

//--------------------------SPI bitbang------------------------------
#define STB_PIN 10
#define CLK_PIN 8 // this will be 7 in the PCB
#define DIO_PIN 11

#define SEGMENT_ARRAY_SIZE 7
static uint16_t display_segments_array[SEGMENT_ARRAY_SIZE];
void initialise_display_data() {
   //clear the display bits. we'll need to have two of these and switch between them 
   for (int i=0; i<SEGMENT_ARRAY_SIZE; i++) {
      display_segments_array[i] = 0;
   }
}

typedef struct {
   uint8_t command;
   uint8_t cmd_idx;
   uint8_t wire_params[32];
   uint8_t wire_params_idx;
} CmdParamSet;
static CmdParamSet cmdparam[100];
static uint8_t cpidx = 0;

void initialise_command(uint8_t idx) {
   //clear the command. we'll need to have two of these and switch between them 
   cmdparam[idx].command = 0;
   cmdparam[idx].cmd_idx = 0;
   cmdparam[idx].wire_params_idx = 0;
   for (int i=0; i<32; i++) {
      cmdparam[idx].wire_params[i] = 0;
   }
}

void handle_clk_rise() {
   if (cpidx >=100) return;
   CmdParamSet *cp = &cmdparam[cpidx];
   if (cp->cmd_idx < 8) {
      cp->command = (cp->command >> 1) | (gpio_get(DIO_PIN) << 7);
      cp->cmd_idx++;
   }
   else {
      int wpi = cp->wire_params_idx/8;
      cp->wire_params[wpi] = (cp->wire_params[wpi] >> 1) | (gpio_get(DIO_PIN) << 7);
      cp->wire_params_idx++;
   }
}
void display_cmd_and_params() {
   char cmd_str[32];
   uint8_t cmd_int;

   for (int i=0; i<100; i++) {
      if (cmdparam[i].cmd_idx == 8) {
         cmd_int = cmdparam[i].command & 0xC0;
         switch(cmd_int) {
            case 0x00:
               uint8_t cmd_param = cmdparam[i].command & 0x03;
               if (cmd_param == 2) {
                  sprintf(cmd_str, "D Mode, 6/12: %x", cmdparam[i].command);
               } else if (cmd_param == 3) {
                  sprintf(cmd_str, "D Mode, 7/11: %x", cmdparam[i].command);
               } else {
                  sprintf(cmd_str, "D Mode, invalid value: %x", cmdparam[i].command);
               }
               break;
            case 0x40:
               cmd_param = cmdparam[i].command & 0x03;
               if (cmd_param == 0) {
                  strcpy(cmd_str, "Data Mode, Write");
               } else if (cmd_param == 2) {
                  sprintf(cmd_str, "Data Mode, Read Keys: %x", cmdparam[i].command);
               } else {
                  sprintf(cmd_str, "Data Mode, Invalid value %x", cmdparam[i].command);
               }
               break;
            case 0x80:
               sprintf(cmd_str, "Display Control: %x", cmdparam[i].command);
               break;
            case 0xC0:
               cmd_param = cmdparam[i].command & 0x0F;  
               sprintf(cmd_str, "Display Address %x", cmd_param);
               break;
         }
         mp_printf(MP_PYTHON_PRINTER, "%s\n", cmd_str);
      }
      else {
         mp_printf(MP_PYTHON_PRINTER, "Not enough bits in command: %d\n", cmdparam[i].cmd_idx);
      }
   }
}

static int debounce_max = 2;
void cooker_handler() {
   bool clk_pin_val = 0;
   bool collecting_data = 0; 
   bool prev_clk_val = 0;
   bool edge_rise = 0;
   bool edge_fall = 0;
   int debounce_count = 0;

   initialise_display_data();
   for (int i=0; i<100; i++) {
      initialise_command(i);
   }

   while(1) {
      if (gpio_get(STB_PIN)) {
         if (collecting_data) {
            if (debounce_count > debounce_max) {
               collecting_data = 0;
               debounce_count = 0;

               // STB has definitely gone high, handle the command and clear out data structures here
               //initialise_command();
               cpidx++;
               if (cpidx == 100) {
                  display_cmd_and_params();
               }
            }
            else {
               // STB has gone high, but could be just noise, let's wait and see...
               debounce_count += 1;
            }
         }
      }
      else { //STB is low
         debounce_count = 0;
         if (!collecting_data) {
            collecting_data = 1;
         }
         // STB is low
         clk_pin_val = gpio_get(CLK_PIN);
         if (prev_clk_val != clk_pin_val) {
            edge_rise = clk_pin_val;
            edge_fall = !clk_pin_val;
            prev_clk_val = clk_pin_val;
         }
         else {
            edge_rise = 0;
            edge_fall = 0;
         }

         if (edge_rise) {
            handle_clk_rise();
         }
      } // STB is low
   }
   // this will never execute
   printf("edge_rise is %d, edge_fall is %d\n", edge_rise, edge_fall);
}

// called from micropython interpreter initialisation
void cobot_irq_init(void) {
    gpio_init(STB_PIN);
    gpio_set_dir(STB_PIN, GPIO_IN);
    gpio_init(CLK_PIN);
    gpio_set_dir(CLK_PIN, GPIO_IN);
    gpio_init(DIO_PIN);
    gpio_set_dir(DIO_PIN, GPIO_IN);

    multicore_launch_core1(cooker_handler);
}

//----------------------debug methods-------------------------
// This is the function which will be called from Python as cexample.print_intr().
// not needed, but kept here as template for methods to exchange data with python code
static mp_obj_t example_print_intr(mp_obj_t idx_obj) {
    int db = mp_obj_get_int(idx_obj);
    //return mp_obj_new_int(interrupt_var[idx]);
    debounce_max = db;
    return mp_obj_new_int(db);
}

// Define a Python reference to the function above.
static MP_DEFINE_CONST_FUN_OBJ_1(example_print_intr_obj, example_print_intr);

//------------------------------------------------------------

// Define all attributes of the module.
// Table entries are key/value pairs of the attribute name (a string)
// and the MicroPython object reference.
// All identifiers and strings are written as MP_QSTR_xxx and will be
// optimized to word-sized integers by the build system (interned strings).
static const mp_rom_map_elem_t example_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_cexample) },
    { MP_ROM_QSTR(MP_QSTR_print_intr), MP_ROM_PTR(&example_print_intr_obj) },
};
static MP_DEFINE_CONST_DICT(example_module_globals, example_module_globals_table);

// Define module object.
const mp_obj_module_t example_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&example_module_globals,
};

// Register the module to make it available in Python.
MP_REGISTER_MODULE(MP_QSTR_cexample, example_user_cmodule);
