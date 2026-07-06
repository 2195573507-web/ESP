#ifndef CSI_PHASE_A_TESTS_H
#define CSI_PHASE_A_TESTS_H

/**
 * @file csi_phase_a_tests.h
 * @brief 阶段 A CSI 离线测试入口声明。
 *
 * 这些函数只在人工验收或主机侧临时编译时显式调用。固件启动链路不会调用它们，
 * 因此不会启动 CSI 常驻任务，也不会访问 S3/Server/Dashboard。
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool csi_feature_test(char *summary, size_t summary_size);

bool csi_feature_boundary_test(char *summary, size_t summary_size);

bool csi_feature_payload_test(char *summary, size_t summary_size);

bool csi_phase_a_run_offline_tests(char *summary, size_t summary_size);

#ifdef __cplusplus
}
#endif

#endif /* CSI_PHASE_A_TESTS_H */
