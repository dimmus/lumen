import lx.shell;

int main(int argc, char* argv[]) {
    lx::shell::shell_app shell{};
    return shell.run(argc, argv);
}
