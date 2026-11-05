import 'vue-router';

import type { Component, DefineComponent, FunctionalComponent } from 'vue';

import type { LocalizedTitle } from '@/locales';

export {};
declare module 'vue-router' {
  interface RouteMeta {
    title?: LocalizedTitle;
    icon?: string | Component | FunctionalComponent | DefineComponent;
    expanded?: boolean;
    orderNo?: number;
    hidden?: boolean;
    hiddenBreadcrumb?: boolean;
    single?: boolean;
    keepAlive?: boolean;
    frameSrc?: string;
    frameBlank?: boolean;
    /** 权限点：当前用户需具备此权限点才显示该菜单（超管 system.all 豁免） */
    perm?: string;
    /** 允许访问的角色（权限点的粗粒度补充） */
    roles?: string[];
    // roleCode?: string; // 前端 roles 控制菜单权限
  }
}
