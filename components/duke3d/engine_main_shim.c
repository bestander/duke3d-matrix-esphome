/*
 * engine_main_shim.c
 *
 * The Duke3D engine entry point is main() in game.c. ESP-IDF uses app_main()
 * so main() is free. duke3d_component.cpp (when DUKE3D_ENGINE_PRESENT is defined)
 * calls duke3d_main(); this shim bridges it to the engine's main().
 */
extern int main(int argc, char **argv);

int duke3d_main(int argc, char **argv)
{
    return main(argc, argv);
}
