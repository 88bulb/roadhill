set(COMPONENT_SRCS "test_main_trans.c"
                   "default.c tools.c message.c parser.c"
                   "ota.c tcp.c player.c ble_adv_scan.c")
set(COMPONENT_ADD_INCLUDEDIRS .)

register_component()
