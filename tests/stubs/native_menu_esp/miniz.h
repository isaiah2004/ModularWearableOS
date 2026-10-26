/* Host stand-in for the ESP ROM tinfl API: tests/native_menu_esp_test.c implements it over a host zlib. */
#include <stddef.h>
#include <stdint.h>
typedef struct {unsigned state;} tinfl_decompressor;
typedef int tinfl_status;
#define tinfl_init(p) ((p)->state=0)
#define TINFL_STATUS_DONE 0
#define TINFL_FLAG_PARSE_ZLIB_HEADER 1
#define TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF 4
tinfl_status tinfl_decompress(tinfl_decompressor *,const uint8_t *,size_t *,uint8_t *,uint8_t *,size_t *,uint32_t);
