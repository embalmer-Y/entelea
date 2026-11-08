/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ENTELEA_CONFIG_MATRIX_H
#define ENTELEA_CONFIG_MATRIX_H

/* R13 配置装配合同：框架配置=可裁剪组合（媒体/经历/情感独立可选——
 * 不强制全开）；产品=MCU 桌面伴侣（启用经历/交互/适用媒体——产品
 * 验收归 owner 硬件批）。本头=机制面配置声明（编译期组合）。 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	bool storage;    /* R03 存储面 */
	bool assistant;  /* R08 辅助闭环 */
	bool media;      /* R09 媒体接口 */
	bool memory;     /* R10 经历记忆 */
	bool affect;     /* R11 情感/适应 */
	bool delivery;   /* R12 试用/晋升 */
} entelea_config_t;

/* 最小辅助配置：无经历/无情感/无媒体（独立 App 照常——真实依赖面）。 */
#define ENTELEA_CONFIG_MINIMAL_ASSIST ((entelea_config_t){ \
		.storage = true,  .assistant = true, \
		.media = false,   .memory = false, \
		.affect = false,  .delivery = true})

/* 完整模拟配置（机制验证面——非产品验收）。 */
#define ENTELEA_CONFIG_FULL_SIMULATED ((entelea_config_t){ \
		.storage = true,  .assistant = true, \
		.media = true,    .memory = true, \
		.affect = true,   .delivery = true})

/** 配置自洽检查：assistant 无 storage→不可持久（E_STATE——合同）。 */
bool entelea_config_valid(const entelea_config_t *c);

/** 配置名（观测）。 */
const char *entelea_config_name(const entelea_config_t *c);

#endif
