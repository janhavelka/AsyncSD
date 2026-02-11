#pragma once

using portMUX_TYPE = int;

#ifndef portMUX_INITIALIZER_UNLOCKED
#define portMUX_INITIALIZER_UNLOCKED 0
#endif

#ifndef portENTER_CRITICAL
#define portENTER_CRITICAL(mux) (void)(mux)
#endif

#ifndef portEXIT_CRITICAL
#define portEXIT_CRITICAL(mux) (void)(mux)
#endif
