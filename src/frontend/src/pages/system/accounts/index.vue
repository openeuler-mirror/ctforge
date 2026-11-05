<template>
  <div>
    <t-card :bordered="false">
      <template #title>账户管理</template>
      <template #actions>
        <t-button theme="primary" @click="openCreate">新建账户</t-button>
      </template>

      <t-table :data="accounts" :columns="columns" :loading="loading" row-key="username" hover>
        <template #role="{ row }">
          <t-tag :theme="roleTheme(row.role)" variant="light">{{ roleText(row.role) }}</t-tag>
        </template>
        <template #status="{ row }">
          <t-tag v-if="row.disabled" theme="danger" variant="light">已禁用</t-tag>
          <t-tag v-else theme="success" variant="light">正常</t-tag>
          <t-tag v-if="row.must_change" theme="warning" variant="outline" style="margin-left: 6px">待改密</t-tag>
        </template>
        <template #created="{ row }">{{ formatTime(row.created_at) }}</template>
        <template #last="{ row }">{{ row.last_login ? formatTime(row.last_login) : '-' }}</template>
        <template #op="{ row }">
          <t-space break-line>
            <t-link theme="primary" hover="color" @click="openEdit(row)">编辑</t-link>
            <t-link theme="warning" hover="color" @click="onToggle(row)">{{ row.disabled ? '启用' : '禁用' }}</t-link>
            <t-link v-if="row.username !== 'admin'" theme="danger" hover="color" @click="onDelete(row)">删除</t-link>
          </t-space>
        </template>
      </t-table>
    </t-card>

    <!-- 新建 -->
    <t-dialog v-model:visible="showCreate" header="新建账户" :on-confirm="doCreate" :confirm-btn="{ loading: saving }">
      <t-form :data="createForm" label-width="90px">
        <t-form-item label="用户名"><t-input v-model="createForm.username" placeholder="至少 3 个字符" /></t-form-item>
        <t-form-item label="密码"><t-input v-model="createForm.password" type="password" placeholder="至少 6 个字符" /></t-form-item>
        <t-form-item label="角色">
          <t-select v-model="createForm.role">
            <t-option value="admin" label="管理员（全部权限）" />
            <t-option value="operator" label="运维（可操作，不能管账户）" />
            <t-option value="viewer" label="只读" />
          </t-select>
        </t-form-item>
        <t-form-item label="显示名"><t-input v-model="createForm.display_name" placeholder="可选" /></t-form-item>
      </t-form>
    </t-dialog>

    <!-- 编辑 -->
    <t-dialog v-model:visible="showEdit" :header="`编辑账户 ${editing?.username ?? ''}`" :on-confirm="doUpdate" :confirm-btn="{ loading: saving }">
      <t-form :data="editForm" label-width="90px">
        <t-form-item label="角色">
          <t-select v-model="editForm.role">
            <t-option value="admin" label="管理员" />
            <t-option value="operator" label="运维" />
            <t-option value="viewer" label="只读" />
          </t-select>
        </t-form-item>
        <t-form-item label="重置密码"><t-input v-model="editForm.password" type="password" placeholder="留空则不修改" /></t-form-item>
      </t-form>
    </t-dialog>
  </div>
</template>

<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { createAccount, deleteAccount, listAccounts, updateAccount, type Account } from '@/api/agent';

const accounts = ref<Account[]>([]);
const loading = ref(false);
const saving = ref(false);
const showCreate = ref(false);
const showEdit = ref(false);
const createForm = ref({ username: '', password: '', role: 'operator', display_name: '' });
const editForm = ref<{ username?: string; role: string; password: string }>({ role: 'viewer', password: '' });
const editing = ref<Account | null>(null);

const columns: PrimaryTableCol[] = [
  { colKey: 'username', title: '用户名', width: 160 },
  { colKey: 'display_name', title: '显示名', width: 180, ellipsis: true },
  { colKey: 'role', title: '角色', width: 130 },
  { colKey: 'status', title: '状态', width: 180 },
  { colKey: 'created', title: '创建时间', width: 170 },
  { colKey: 'last', title: '最后登录', width: 170 },
  { colKey: 'op', title: '操作', width: 170, fixed: 'right' },
];

function roleTheme(r: string): 'danger' | 'warning' | 'primary' | 'default' {
  const m: Record<string, 'danger' | 'warning' | 'primary'> = { admin: 'danger', operator: 'warning', viewer: 'primary' };
  return m[r] ?? 'default';
}
function roleText(r: string) {
  return { admin: '管理员', operator: '运维', viewer: '只读' }[r] || r;
}
function formatTime(t: string) {
  return t && t !== '0001-01-01T00:00:00Z' ? dayjs(t).format('YYYY-MM-DD HH:mm') : '-';
}

async function fetchAll() {
  loading.value = true;
  try {
    accounts.value = await listAccounts();
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
  } finally {
    loading.value = false;
  }
}

function openCreate() {
  createForm.value = { username: '', password: '', role: 'operator', display_name: '' };
  showCreate.value = true;
}

async function doCreate() {
  saving.value = true;
  try {
    await createAccount(createForm.value);
    MessagePlugin.success('创建成功');
    showCreate.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '创建失败');
  } finally {
    saving.value = false;
  }
}

function openEdit(row: Account) {
  editing.value = row;
  editForm.value = { username: row.username, role: row.role, password: '' };
  showEdit.value = true;
}

async function doUpdate() {
  if (!editing.value) return;
  saving.value = true;
  try {
    const data: any = { role: editForm.value.role };
    if (editForm.value.password) data.password = editForm.value.password;
    await updateAccount(editing.value.username, data);
    MessagePlugin.success('已保存');
    showEdit.value = false;
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

function onToggle(row: Account) {
  const target = !row.disabled;
  const dlg = DialogPlugin.confirm({
    header: target ? '禁用账户' : '启用账户',
    body: `确定${target ? '禁用' : '启用'} ${row.username} 吗？${target ? '该账户当前会话将被立即踢出。' : ''}`,
    theme: target ? ('warning' as const) : undefined,
    onConfirm: async () => {
      try {
        await updateAccount(row.username, { disabled: target });
        MessagePlugin.success('操作成功');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '操作失败');
      }
      dlg.destroy();
    },
  });
}

function onDelete(row: Account) {
  const dlg = DialogPlugin.confirm({
    header: '删除账户',
    body: `确定删除 ${row.username} 吗？此操作不可恢复。`,
    theme: 'danger',
    onConfirm: async () => {
      try {
        await deleteAccount(row.username);
        MessagePlugin.success('已删除');
        fetchAll();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

onMounted(fetchAll);
</script>
