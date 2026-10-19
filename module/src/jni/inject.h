#ifndef ZYGISKFRIDA_INJECT_H
#define ZYGISKFRIDA_INJECT_H

void inject_lib(const char *lib_path, const char *log_context);
int check_and_inject(const char *app_name, const char *app_data_dir);

#endif  // ZYGISKFRIDA_INJECT_H
