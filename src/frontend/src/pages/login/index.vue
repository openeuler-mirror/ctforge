<template>
  <div class="login-wrapper">
    <login-header />

    <div class="login-container">
      <div class="title-container">
        <h1 class="title margin-no">ctforge</h1>
        <h1 class="title">主机监控与运维平台</h1>
      </div>

      <t-form ref="form" class="item-container" :data="formData" :rules="FORM_RULES" label-width="0" @submit="onSubmit">
        <t-form-item name="account">
          <t-input v-model="formData.account" size="large" placeholder="用户名">
            <template #prefix-icon><t-icon name="user" /></template>
          </t-input>
        </t-form-item>

        <t-form-item name="password">
          <t-input
            v-model="formData.password"
            size="large"
            :type="showPsw ? 'text' : 'password'"
            clearable
            placeholder="密码"
            @enter="onSubmit"
          >
            <template #prefix-icon><t-icon name="lock-on" /></template>
            <template #suffix-icon>
              <t-icon :name="showPsw ? 'browse' : 'browse-off'" @click="showPsw = !showPsw" />
            </template>
          </t-input>
        </t-form-item>

        <t-form-item class="btn-container">
          <t-button block size="large" type="submit" :loading="loading">登 录</t-button>
        </t-form-item>
      </t-form>
    </div>

    <footer class="copyright">ctforge · Linux 主机集中监控与运维</footer>
  </div>
</template>

<script setup lang="ts">
import { ref } from 'vue';
import { useRouter } from 'vue-router';
import { MessagePlugin } from 'tdesign-vue-next';

import { login } from '@/api/agent';

import LoginHeader from './components/Header.vue';
import { saveAuth } from '@/utils/auth';

defineOptions({ name: 'LoginIndex' });

const router = useRouter();
const form = ref();
const loading = ref(false);
const showPsw = ref(false);
const formData = ref({ account: '', password: '' });

const FORM_RULES = {
  account: [{ required: true, message: '请输入用户名', type: 'error' as const }],
  password: [{ required: true, message: '请输入密码', type: 'error' as const }],
};

async function onSubmit(ctx: any) {
  if (ctx?.validateResult === false) return;
  loading.value = true;
  try {
    const res = await login(formData.value.account, formData.value.password);
    saveAuth(res.token, res.username, res.role, res.expires_at);
    MessagePlugin.success('登录成功');
    const redirect = (router.currentRoute.value.query.redirect as string) || '/monitor/agents';
    router.push(redirect);
  } catch (e: any) {
    MessagePlugin.error(e.message || '登录失败');
  } finally {
    loading.value = false;
  }
}
</script>

<style lang="less" scoped>
@import './index.less';
</style>
