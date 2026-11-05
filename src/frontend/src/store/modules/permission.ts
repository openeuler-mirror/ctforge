import cloneDeep from 'lodash/cloneDeep';
import { defineStore } from 'pinia';
import type { RouteRecordRaw } from 'vue-router';

import router, { fixedRouterList, homepageRouterList } from '@/router';
import { filterMenuByPerms, getPermsFromServer } from '@/utils/auth';
import { store } from '@/store';

export const usePermissionStore = defineStore('permission', {
  state: () => ({
    whiteListRouters: ['/login'],
    routers: [] as Array<RouteRecordRaw>,
    removeRoutes: [] as Array<RouteRecordRaw>,
    asyncRoutes: [] as Array<RouteRecordRaw>,
  }),
  actions: {
    // 按当前用户的权限点过滤菜单；无权限的菜单项直接不显示
    async initStaticRoutes() {
      const perms = await getPermsFromServer();
      const all = cloneDeep([...homepageRouterList, ...fixedRouterList]);
      this.routers = filterMenuByPerms(all);
    },
    async initRoutes() {
      await this.initStaticRoutes();
    },
    async buildAsyncRoutes() {
      await this.initStaticRoutes();
      return [];
    },
    async restoreRoutes() {
      this.asyncRoutes = [];
    },
  },
});

export function getPermissionStore() {
  return usePermissionStore(store);
}
