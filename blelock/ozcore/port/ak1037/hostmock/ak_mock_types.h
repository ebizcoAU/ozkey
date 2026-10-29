/* ak_mock_types.h — the Anyka base types the port uses, for the host build of
 * port/ak1037 against mocks (ozkey-49 P5-A). Written from the SDK's
 * prototypes, not copied: the SDK itself stays out of the repo. */
#ifndef AK_MOCK_TYPES_H
#define AK_MOCK_TYPES_H

#include <stddef.h>
#include <stdint.h>

typedef uint8_t  T_U8;
typedef uint32_t T_U32;
typedef uint8_t  T_BOOL;
typedef void     T_VOID;
typedef void    *T_pVOID;

#define AK_TRUE  1
#define AK_FALSE 0
#define AK_NULL  ((void *)0)

#endif
