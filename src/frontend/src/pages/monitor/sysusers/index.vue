<template>
  <div>
    <t-card :bordered="false">
      <template #title>
        <t-space align="center">
          <span>系统账户管理</span>
          <t-select v-model="currentAgent" :options="agentOptions" placeholder="选择主机" style="width: 260px" :loading="loadingAgents" @change="fetchUsers" />
        </t-space>
      </template>
      <template #actions>
        <t-button theme="primary" :loading="loading" @click="fetchUsers">刷新</t-button>
      </template>

      <t-alert theme="info" style="margin-bottom: 16px">
        管理被控主机上的 Linux 系统账户。修改密码、锁定/解锁操作需要 agent 以 root 权限运行。
      </t-alert>

      <t-table :data="users" :columns="columns" :loading="loading" row-key="username" hover size="small" :max-height="600">
        <template #locked="{ row }">
          <t-tag v-if="row.locked" theme="danger" variant="light" size="small">已锁定</t-tag>
          <t-tag v-else theme="success" variant="light" size="small">正常</t-tag>
        </template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="onChgPwd(row)">改密</t-link>
            <t-link v-if="row.username !== 'root'" :theme="row.locked ? 'success' : 'warning'" hover="color" @click="onToggleLock(row)">
              {{ row.locked ? '解锁' : '锁定' }}
            </t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 修改密码对话框 -->
    <t-dialog v-model:visible="showPwd" :header="`修改密码：${editing?.username ?? ''}`" :on-confirm="doChgPwd" :confirm-btn="{ loading: saving }">
      <t-alert theme="warning" style="margin-bottom: 12px">
        密码将通过 TLS 加密通道传输到目标主机，执行后不会存储在任何地方。
      </t-alert>
      <t-form :data="pwdForm" label-width="90px">
        <t-form-item label="新密码">
          <t-input v-model="pwdForm.password" type="password" placeholder="至少 6 个字符" />
        </t-form-item>
        <t-form-item label="确认密码">
          <t-input v-model="pwdForm.confirm" type="password" placeholder="再次输入新密码" />
        </t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { useRoute } from 'vue-router';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';

import { listAgents, listSysUsers, changeSysUserPassword, lockSysUser, type SysUser } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);
const loading = ref(false);
const saving = ref(false);
const users = ref<SysUser[]>([]);
const showPwd = ref(false);
const editing = ref<SysUser | null>(null);
const pwdForm = ref({ password: '', confirm: '' });

const agentOptions = computed(() => agents.value);

const columns: PrimaryTableCol[] = [
  { colKey: 'username', title: '用户名', width: 140 },
  { colKey: 'uid', title: 'UID', width: 80 },
  { colKey: 'gid', title: 'GID', width: 80 },
  { colKey: 'home', title: '主目录', width: 180, ellipsis: true },
  { colKey: 'shell', title: 'Shell', width: 140, ellipsis: true },
  { colKey: 'locked', title: '状态', width: 90 },
  { colKey: 'last_login', title: '最近登录', width: 160, ellipsis: true },
  { colKey: 'op', title: '操作', width: 120, fixed: 'right' },
];

async function loadAgents() {
  loadingAgents.value = true;
  try {
    const list = await listAgents();
    // 默认跳过离线主机
    agents.value = list
      .filter((a) => a.online)
      .map((a) => ({
        label: `${a.hostname} (${a.ip})`,
        value: a.agent_id,
      }));
    if (!currentAgent.value && agents.value.length) currentAgent.value = agents.value[0].value;
  } finally {
    loadingAgents.value = false;
  }
}

async function fetchUsers() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    users.value = await listSysUsers(currentAgent.value);
  } catch (e: any) {
    MessagePlugin.error(e.message || '查询失败');
  } finally {
    loading.value = false;
  }
}

function onChgPwd(row: SysUser) {
  editing.value = row;
  pwdForm.value = { password: '', confirm: '' };
  showPwd.value = true;
}

async function doChgPwd() {
  if (!editing.value) return;
  if (pwdForm.value.password !== pwdForm.value.confirm) {
    MessagePlugin.error('两次密码输入不一致');
    return;
  }
  if (pwdForm.value.password.length < 6) {
    MessagePlugin.error('密码至少 6 个字符');
    return;
  }
  saving.value = true;
  try {
    await changeSysUserPassword(currentAgent.value, editing.value.username, pwdForm.value.password);
    MessagePlugin.success('密码已修改');
    showPwd.value = false;
  } catch (e: any) {
    MessagePlugin.error(e.message || '修改失败');
  } finally {
    saving.value = false;
  }
}

function onToggleLock(row: SysUser) {
  const target = !row.locked;
  const dlg = DialogPlugin.confirm({
    header: target ? '锁定账户' : '解锁账户',
    body: `确定${target ? '锁定' : '解锁'} ${row.username} 吗？${target ? '该用户将无法登录系统。' : ''}`,
    theme: target ? ('warning' as const) : undefined,
    onConfirm: async () => {
      try {
        await lockSysUser(currentAgent.value, row.username, target);
        MessagePlugin.success('操作成功');
        fetchUsers();
      } catch (e: any) {
        MessagePlugin.error(e.message || '操作失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(async () => {
  await loadAgents();
  fetchUsers();
});
</script>
