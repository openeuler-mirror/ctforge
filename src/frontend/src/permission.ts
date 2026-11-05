import 'nprogress/nprogress.css';

import NProgress from 'nprogress';
import { MessagePlugin } from 'tdesign-vue-next';

import router from '@/router';
import { getPermissionStore } from '@/store';
import { clearAuth, getToken, hasValidLocalAuth } from '@/utils/auth';

NProgress.configure({ showSpinner: false });

const PUBLIC_PATHS = ['/login'];

router.beforeEach(async (to, from, next) => {
  NProgress.start();

  const permissionStore = getPermissionStore();
  if (permissionStore.routers.length === 0) {
    await permissionStore.initStaticRoutes();
  }

  const token = getToken();

  // 已登录访问登录页 → 回首页
  if (token && to.path === '/login') {
    next({ path: '/', replace: true });
    return;
  }

  if (PUBLIC_PATHS.includes(to.path)) {
    next();
    return;
  }

  if (!token || !hasValidLocalAuth()) {
    clearAuth();
    // 静默跳转，不弹警告（避免刷新页面时打扰）
    next({ path: '/login', query: { redirect: to.fullPath } });
    NProgress.done();
    return;
  }

  // 角色校验：meta.roles 指定允许的角色
  const required = (to.meta?.roles as string[] | undefined) ?? null;
  const role = localStorage.getItem('ctc_role') || 'viewer';
  if (required && !required.includes(role)) {
    next({ path: '/result/403', replace: true });
    NProgress.done();
    return;
  }

  next();
});

router.afterEach(() => {
  NProgress.done();
});
