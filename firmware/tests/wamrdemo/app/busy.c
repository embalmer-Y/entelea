/* SPDX-License-Identifier: Apache-2.0 */
/* Q-23 实验批可控负载（wamrdemo）：确定性算术循环——无内存增长、无导入、
 * 无随机/墙钟（合同 9 友好）。busy(n) 返回校验和防编译器消除循环；
 * n 与宿主耗时线性相关（时延画像/饿死/竞态负载用）。 */
__attribute__((export_name("busy")))
int busy(int n)
{
	unsigned acc = 0x2545F491u;

	for (int i = 0; i < n; i++) {
		acc = acc * 1664525u + 1013904223u + (unsigned)i;
	}
	return (int)(acc & 0x7Fu);
}
