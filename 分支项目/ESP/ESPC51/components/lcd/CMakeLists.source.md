# LCD 是 BSP 组件下的独立子模块。
# 当前 ESP-IDF 工程由 components/BSP/CMakeLists.txt 统一注册 BSP 内部子目录，
# 因此本文件只保留模块说明，不重复调用 idf_component_register()，避免一个目录注册成两个组件。
