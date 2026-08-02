/**
 * @file base_template.h
 *
 * Filthy unicode abuse to make derived types look like templates.
 *
 * Example:
 * ```c
 * #define _nya_derive_hashmap_name(key_type, value_type) nya_template(HashMap, key_type, value_type)
 * #define nya_derive_hashmap(key_type, value_type) \
 *     typedef struct { } _nya_derive_hashmap_name(key_type, value_type);
 *
 * typedef struct {
 * } Entity;
 * derive_hashmap(int, Entity);
 *
 * int main(void) {
 *     HashMapᐸintˏEntityᐳ entities;
 *     return 0;
 * }
 * ```
 * */
#pragma once

#define nya_template(...) _nya_template_pick(__VA_ARGS__, _nya_template4, _nya_template3, _nya_template2, _nya_template1)(__VA_ARGS__)
#define _nya_template_pick(_0, _1, _2, _3, _4, NAME, ...) NAME
#define _nya_template1(base, _1)                          base##ᐸ##_1##ᐳ
#define _nya_template2(base, _1, _2)                      base##ᐸ##_1##ˏ##_2##ᐳ
#define _nya_template3(base, _1, _2, _3)                  base##ᐸ##_1##ˏ##_2##ˏ##_3##ᐳ
#define _nya_template4(base, _1, _2, _3, _4)              base##ᐸ##_1##ˏ##_2##ˏ##_3##ˏ##_4##ᐳ
