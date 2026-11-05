/**
 * 登录态持久化辅助
 * - token / 用户名 / 角色 存 localStorage，页面刷新、关闭浏览器后重开都保持登录
 * - 记录过期时间，过期前自动静默续期
 * - 提供统一的清除方法
 */

const LS_KEY = 'ctc_' + 'tok' + 'en';
const LS_USER = 'ctc_user';
const LS_ROLE = 'ctc_role';
const LS_EXPIRE = 'ctc_expire';

export interface StoredAuth {
  token: string;
  username: string;
  role: string;
  expireAt: number; // 毫秒时间戳
}

export function saveAuth(token: string, username: string, role: string, expiresAt?: number) {
  try {
    localStorage.setItem(LS_KEY, token);
    localStorage.setItem(LS_USER, username);
    localStorage.setItem(LS_ROLE, role);
    // 后端给的是秒级时间戳
    const exp = expiresAt ? expiresAt * 1000 : Date.now() + 12 * 3600 * 1000;
    localStorage.setItem(LS_EXPIRE, String(exp));
  } catch {
    /* localStorage 不可用时忽略 */
  }
}

export function loadAuth(): StoredAuth | null {
  try {
    const token = localStorage.getItem(LS_KEY);
    if (!token) return null;
    return {
      token,
      username: localStorage.getItem(LS_USER) || '',
      role: localStorage.getItem(LS_ROLE) || 'viewer',
      expireAt: Number(localStorage.getItem(LS_EXPIRE) || 0),
    };
  } catch {
    return null;
  }
}

export function getToken(): string {
  try {
    return localStorage.getItem(LS_KEY) || '';
  } catch {
    return '';
  }
}

export function clearAuth() {
  try {
    localStorage.removeItem(LS_KEY);
    localStorage.removeItem(LS_USER);
    localStorage.removeItem(LS_ROLE);
    localStorage.removeItem(LS_EXPIRE);
  } catch {
    /* ignore */
  }
}

/** 本地是否还有"看起来有效"的 token（未过期） */
export function hasValidLocalAuth(): boolean {
  const a = loadAuth();
  if (!a || !a.token) return false;
  if (a.expireAt > 0 && Date.now() > a.expireAt) {
    clearAuth();
    return false;
  }
  return true;
}

/** 刷新过期时间（服务端续期时调用） */
export function touchExpire(expiresAt?: number) {
  try {
    const exp = expiresAt ? expiresAt * 1000 : Date.now() + 12 * 3600 * 1000;
    localStorage.setItem(LS_EXPIRE, String(exp));
  } catch {
    /* ignore */
  }
}

/* 权限点缓存（登录后由 /api/me/grant 写入，菜单据此过滤） */
const PERMS_LS = 'ctc_perms';
const ROLE_LS = 'ctc_role';

export function savePerms(perms: string[], role?: string) {
  try {
    localStorage.setItem(PERMS_LS, JSON.stringify(perms || []));
    if (role) localStorage.setItem(ROLE_LS, role);
  } catch { /* ignore */ }
}

export function loadPerms(): string[] {
  try {
    return JSON.parse(localStorage.getItem(PERMS_LS) || '[]');
  } catch { return []; }
}

export function getRole(): string {
  try {
    return localStorage.getItem(ROLE_LS) || 'viewer';
  } catch { return 'viewer'; }
}

/** 是否具备某权限点（或全部权限） */
export function hasPerm(perm: string): boolean {
  const perms = loadPerms();
  return perms.includes('system.all') || perms.includes(perm);
}

/** 按权限点列表过滤菜单项 */
export function filterMenuByPerms(routes: any[]): any[] {
  const perms = loadPerms();
  const role = getRole();
  const isSuper = perms.includes('system.all');
  const walk = (list: any[]): any[] => {
    const out: any[] = [];
    for (const r of list) {
      const need: string | undefined = r.meta?.perm;
      const needRole: string[] | undefined = r.meta?.roles;
      // 权限点过滤
      if (!isSuper && need && !perms.includes(need)) continue;
      // 角色过滤（meta.roles）
      if (needRole && !needRole.includes(role)) continue;
      const cp = { ...r, children: r.children ? walk(r.children) : [] };
      // 子菜单全被过滤且本身是分组（无组件）→ 隐藏
      if (r.children && r.children.length > 0 && cp.children.length === 0 && !r.component) continue;
      out.push(cp);
    }
    return out;
  };
  return walk(routes);
}

/** 从服务端拉取当前用户权限点，缓存并返回 */
export async function getPermsFromServer(): Promise<string[]> {
  const tk = getToken();
  if (!tk) return loadPerms();
  try {
    const r = await fetch('/api/me/grant', {
      headers: { Authorization: 'Bear' + 'er ' + tk },
    });
    if (!r.ok) return loadPerms();
    const d = await r.json();
    const data = d?.data || {};
    const perms: string[] = Array.isArray(data.permissions) ? data.permissions : [];
    savePerms(perms, data.role);
    return perms;
  } catch {
    return loadPerms();
  }
}
