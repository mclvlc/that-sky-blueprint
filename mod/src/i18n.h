#ifndef __BP_I18N_H__
#define __BP_I18N_H__

// 面板语言：0=English（默认），1=简体中文
extern int gUiLang;

// 从 <data>/ui.cfg 读取语言
void bpI18nInit(void);

// 切换并保存语言
void bpI18nSetLang(int lang);

// 返回当前语言对应的字符串
const char *tr(const char *en, const char *zh);

#endif
