<template>
  <div :class="layoutCls">
    <t-head-menu :class="menuCls" :theme="menuTheme" expand-type="popup" :value="active">
      <template #logo>
        <span v-if="showLogo" class="header-logo-container" @click="handleNav('/monitor/agents')">
          <logo-full class="t-logo" />
        </span>
        <div v-else class="header-operate-left">
                </div>
      </template>
      <template v-if="layout !== 'side'" #default>
        <menu-content class="header-menu" :nav-data="menu" />
      </template>
      <template #operations>
        <div class="operations-container">
          <!-- 搜索框 -->
        
          <!-- 全局通知 -->
          <notice />


          <t-dropdown :min-column-width="120" trigger="click">
            <template #dropdown>
              <t-dropdown-item class="operations-dropdown-container-item" @click="showPwd = true">
                <user-circle-icon />修改密码
              </t-dropdown-item>
              <t-dropdown-item class="operations-dropdown-container-item" @click="handleLogout">
                <poweroff-icon />{{ t('layout.header.signOut') }}
              </t-dropdown-item>
            </template>
            <t-button class="header-user-btn" theme="default" variant="text">
              <template #icon>
                <t-icon class="header-user-avatar" name="user-circle" />
              </template>
              <div class="header-user-account">{{ user.userInfo.name }}</div>
              <template #suffix><chevron-down-icon /></template>
            </t-button>
          </t-dropdown>

        </div>
      </template>
    </t-head-menu>

    <!-- 修改密码 -->
    <t-dialog v-model:visible="showPwd" header="修改密码" :on-confirm="doChangePwd" :confirm-btn="{ loading: pwdSaving }">
      <t-form :data="pwdForm" label-width="90px">
        <t-form-item label="当前密码"><t-input v-model="pwdForm.old" type="password" /></t-form-item>
        <t-form-item label="新密码"><t-input v-model="pwdForm.new1" type="password" placeholder="至少 6 位" /></t-form-item>
        <t-form-item label="确认新密码"><t-input v-model="pwdForm.new2" type="password" /></t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>
<script setup lang="ts">
import { ChevronDownIcon, PoweroffIcon, UserCircleIcon } from 'tdesign-icons-vue-next';
import type { PropType } from 'vue';
import { computed, ref } from 'vue';
import { useRouter } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';

import LogoFull from '@/assets/assets-logo-full.svg?component';
import { prefix } from '@/config/global';
import { t } from '@/locales';
import { getActive } from '@/router';
import { useUserStore } from '@/store';
import type { MenuRoute, ModeType } from '@/types/interface';

import MenuContent from './MenuContent.vue';
import Notice from './Notice.vue';

const { theme, layout, showLogo, menu, isFixed, isCompact } = defineProps({
  theme: {
    type: String,
    default: 'light',
  },
  layout: {
    type: String,
    default: 'top',
  },
  showLogo: {
    type: Boolean,
    default: true,
  },
  menu: {
    type: Array as PropType<MenuRoute[]>,
    default: () => [],
  },
  isFixed: {
    type: Boolean,
    default: false,
  },
  isCompact: {
    type: Boolean,
    default: false,
  },
  maxLevel: {
    type: Number,
    default: 3,
  },
});

const router = useRouter();
const user = useUserStore();


const active = computed(() => getActive());

const layoutCls = computed(() => [`${prefix}-header-layout`]);

const menuCls = computed(() => {
  return [
    {
      [`${prefix}-header-menu`]: !isFixed,
      [`${prefix}-header-menu-fixed`]: isFixed,
      [`${prefix}-header-menu-fixed-side`]: layout === 'side' && isFixed,
      [`${prefix}-header-menu-fixed-side-compact`]: layout === 'side' && isFixed && isCompact,
    },
  ];
});
const menuTheme = computed(() => theme as ModeType);

const handleNav = (url: string) => {
  router.push(url);
};

// ---- 修改密码 ----
const showPwd = ref(false);
const pwdSaving = ref(false);
const pwdForm = ref({ old: '', new1: '', new2: '' });

async function doChangePwd() {
  if (!pwdForm.value.old || !pwdForm.value.new1) {
    MessagePlugin.warning('请填写完整');
    return;
  }
  if (pwdForm.value.new1 !== pwdForm.value.new2) {
    MessagePlugin.error('两次新密码不一致');
    return;
  }
  if (pwdForm.value.new1.length < 6) {
    MessagePlugin.error('新密码至少 6 位');
    return;
  }
  pwdSaving.value = true;
  try {
    const { changePassword } = await import('@/api/agent');
    await changePassword(pwdForm.value.old, pwdForm.value.new1);
    MessagePlugin.success('密码已修改，请重新登录');
    showPwd.value = false;
    pwdForm.value = { old: '', new1: '', new2: '' };
    setTimeout(() => handleLogout(), 800);
  } catch (e: any) {
    MessagePlugin.error(e.message || '修改失败');
  } finally {
    pwdSaving.value = false;
  }
}

const handleLogout = () => {
  user.logout();
  import('@/utils/auth').then(({ clearAuth }) => clearAuth());
  router.push({
    path: '/login',
    query: { redirect: router.currentRoute.value.fullPath },
  });
};


</script>
<style lang="less" scoped>
.@{starter-prefix}-header {
  &-menu-fixed {
    position: fixed;
    top: 0;
    z-index: 1001;

    :deep(.t-head-menu__inner) {
      padding-right: var(--td-comp-margin-xl);
    }

    &-side {
      left: 232px;
      right: 0;
      z-index: 10;
      width: auto;
      transition: all 0.3s;

      &-compact {
        left: 64px;
      }
    }
  }

  &-logo-container {
    cursor: pointer;
    display: inline-flex;
  }
}

.header-menu {
  flex: 1 1 auto;
  display: inline-flex;

  :deep(.t-menu__item) {
    min-width: unset;
  }
}

.operations-container {
  display: flex;
  align-items: center;

  .t-popup__reference {
    display: flex;
    align-items: center;
    justify-content: center;
  }

  .t-button {
    margin-left: var(--td-comp-margin-l);
  }
}

.header-operate-left {
  display: flex;
  align-items: normal;
  line-height: 0;
}

.header-logo-container {
  width: 184px;
  height: 26px;
  display: flex;
  margin-left: 24px;
  color: var(--td-text-color-primary);

  .t-logo {
    width: 100%;
    height: 100%;

    &:hover {
      cursor: pointer;
    }
  }

  &:hover {
    cursor: pointer;
  }
}

.header-user-account {
  display: inline-flex;
  align-items: center;
  color: var(--td-text-color-primary);
}

:deep(.t-head-menu__inner) {
  border-bottom: 1px solid var(--td-component-stroke);
}

.t-menu--light {
  .header-user-account {
    color: var(--td-text-color-primary);
  }
}

.t-menu--dark {
  .t-head-menu__inner {
    border-bottom: 1px solid var(--td-gray-color-10);
  }

  .header-user-account {
    color: rgb(255 255 255 / 55%);
  }
}

.operations-dropdown-container-item {
  width: 100%;
  display: flex;
  align-items: center;

  :deep(.t-dropdown__item-text) {
    display: flex;
    align-items: center;
  }

  .t-icon {
    font-size: var(--td-comp-size-xxxs);
    margin-right: var(--td-comp-margin-s);
  }

  :deep(.t-dropdown__item) {
    width: 100%;
    margin-bottom: 0;
  }

  &:last-child {
    :deep(.t-dropdown__item) {
      margin-bottom: 8px;
    }
  }
}
</style>
<!-- eslint-disable-next-line vue-scoped-css/enforce-style-type -->
<style lang="less">
.operations-dropdown-container-item {
  .t-dropdown__item-text {
    display: flex;
    align-items: center;
  }
}
</style>
