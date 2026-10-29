/* com_msh.h (host mock): a command export registers the command by name, so
 * a test can run it as the shell would (mock_msh_run). */
#ifndef COM_MSH_H_MOCK
#define COM_MSH_H_MOCK
typedef void (*mock_msh_fn)(char argc, char **argv);
void mock_msh_register(const char *name, mock_msh_fn fn);
#define AK_MSH_CMD_EXPORT(func, command, desc)                                  \
    __attribute__((constructor)) static void mock_msh_reg_##func(void)         \
    { mock_msh_register(#command, func); }
#endif
