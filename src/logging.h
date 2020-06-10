#ifndef LOGGING_H
#define LOGGING_H

#include <stdarg.h>

#define LOG_ERROR 0	  /* error conditions                     */
#define LOG_WARNING 1 /* warning conditions                   */
#define LOG_NOTICE 2  /* normal but significant condition     */
#define LOG_INFO 3	  /* informational                        */
#define LOG_VERBOSE 4 /* verbose                              */
#define LOG_DEBUG 5	  /* debug-level messages                 */

namespace logging
{

extern char print_level;
extern char error_level;

int print_flagged(const unsigned int flag, const char *fmt, va_list args);
int vprint(const char *fmt, va_list args);
int print(const char *fmt, ...);
int print(const unsigned int log_level, const char *fmt, ...);
int print_master(const char *fmt, ...);
int print_master(const unsigned int log_level, const char *fmt, ...);


int master_error(const char *fmt, ...);
int master_warning(const char *fmt, ...);
int master_notice(const char *fmt, ...);
int master_info(const char *fmt, ...);
int master_verbose(const char *fmt, ...);
int master_debug(const char *fmt, ...);

void print_runtime_info(unsigned int output_number,
			unsigned int time_step_coarse, double dt);
void print_runtime_final();
void start_timer();

} // namespace logging

#endif // LOGGING_H
