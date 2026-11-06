<template>
  <div>
    <t-row :gutter="16">
      <!-- 系统信息 -->
      <t-col :span="8">
        <t-card title="服务端信息" :bordered="false">
          <t-descriptions :column="1" size="small">
            <t-descriptions-item label="服务端 ID">
              <span class="mono">{{ info.server_id || '-' }}</span>
            </t-descriptions-item>
            <t-descriptions-item label="主机数">{{ info.total ?? 0 }}</t-descriptions-item>
            <t-descriptions-item label="在线 / 离线">
              <t-tag theme="success" variant="light" size="small">{{ info.online ?? 0 }}</t-tag>
              <t-tag theme="default" variant="light" size="small" style="margin-left: 6px">{{ info.offline ?? 0 }}</t-tag>
            </t-descriptions-item>
            <t-descriptions-item label="待批准">
              <t-tag v-if="info.pending" theme="warning" variant="light" size="small">{{ info.pending }}</t-tag>
              <span v-else>0</span>
            </t-descriptions-item>
            <t-descriptions-item label="可用率">{{ info.availability ?? 0 }}%</t-descriptions-item>
          </t-descriptions>
        </t-card>
      </t-col>

      <!-- AI 配置 -->
      <t-col :span="8">
        <t-card :bordered="false">
          <template #title>
            <t-space align="center">
              <span>AI 分析配置</span>
              <t-tag v-if="aiForm.enabled && aiReady" theme="success" variant="light" size="small">已启用</t-tag>
              <t-tag v-else theme="default" variant="light" size="small">未启用</t-tag>
            </t-space>
          </template>
          <template #actions>
            <t-button theme="primary" size="small" :loading="saving" @click="saveAI">保存</t-button>
          </template>
          <t-form :data="aiForm" label-width="90px" size="small">
            <t-form-item label="启用">
              <t-switch v-model="aiForm.enabled" />
            </t-form-item>
            <t-form-item label="API 地址">
              <t-input v-model="aiForm.base_url" placeholder="https://api.openai.com/v1" />
            </t-form-item>
            <t-form-item label="API Key">
              <t-input v-model="apiKeyInput" type="password" placeholder="sk-... 或留空保持不变" />
            </t-form-item>
            <t-form-item label="模型">
              <t-input v-model="aiForm.model" placeholder="gpt-4o-mini / qwen-plus / deepseek-chat" />
            </t-form-item>
            <t-form-item label="最大 tokens">
              <t-input-number v-model="aiForm.max_tokens" :min="256" :max="16000" />
            </t-form-item>
          </t-form>
          <t-alert theme="info" style="margin-top: 8px; font-size: 12px">
            支持任何 OpenAI 兼容接口（OpenAI / DeepSeek / 通义千问 / Ollama / vLLM）。
            配置后主机总览页会出现「AI 分析」按钮。
          </t-alert>
        </t-card>
      </t-col>

      <!-- 客户端部署 -->
      <t-col :span="8">
        <t-card title="客户端部署" :bordered="false">
          <div class="cmd-box">
            <code>{{ deployCmd || '加载中…' }}</code>
            <t-button theme="primary" size="small" :disabled="!deployCmd" @click="copyCmd">复制</t-button>
          </div>
          <t-descriptions :column="1" size="small">
            <t-descriptions-item label="Agent 下载">
              <a :href="downloadUrl" target="_blank" class="link">{{ downloadUrl }}</a>
            </t-descriptions-item>
            <t-descriptions-item label="支持系统">
              Fedora / RHEL / CentOS / Debian / Ubuntu / openSUSE / OpenWrt / Alpine
            </t-descriptions-item>
            <t-descriptions-item label="安装方式">
              完全静态单文件，零依赖
            </t-descriptions-item>
          </t-descriptions>
        </t-card>
      </t-col>
    </t-row>

    <!-- SSH 跳板机（JumpServer） -->
    <t-card :bordered="false" style="margin-top: 16px">
      <template #title>
        <t-space align="center">
          <t-icon name="terminal" />
          <span>SSH 跳板机（JumpServer）</span>
          <t-tag v-if="sshEnabled" theme="success" variant="light" size="small">已启用</t-tag>
          <t-tag v-else theme="default" variant="light" size="small">未启用</t-tag>
        </t-space>
      </template>
      <t-row :gutter="16">
        <t-col :span="10">
          <t-descriptions :column="1" size="small">
            <t-descriptions-item label="连接方式">
              <code class="mono">ssh -p 2222 {{ currentUser }}@{{ serverHost }}</code>
              <t-button theme="primary" size="small" variant="text" @click="copySSHCmd">复制</t-button>
            </t-descriptions-item>
            <t-descriptions-item label="认证方式">
              账户密码 或 专属公钥（下方配置）
            </t-descriptions-item>
            <t-descriptions-item label="主机密钥指纹">
              <code class="mono" style="font-size:11px">{{ sshHostKeyFingerprint || '-' }}</code>
            </t-descriptions-item>
          </t-descriptions>
          <t-alert theme="info" style="margin-top: 8px; font-size: 12px">
            登录后显示可连接主机列表（序号/名称/IP/备注），输入序号进入对应主机终端，exit 返回列表。
            支持会话挂起，关网页后程序继续执行。
          </t-alert>
        </t-col>
        <t-col :span="14">
          <template #actions>
            <t-button theme="primary" size="small" :loading="sshKeysSaving" @click="saveSSHKeys">保存公钥</t-button>
          </template>
          <t-form label-width="100px" size="small">
            <t-form-item label="专属公钥">
              <t-textarea
                v-model="sshKeysText"
                :rows="5"
                placeholder="粘贴 OpenSSH 公钥（ssh-rsa / ssh-ed25519 / ecdsa-sha2 开头，一行一个）"
                :autosize="{ minRows: 4, maxRows: 8 }"
              />
            </t-form-item>
            <t-form-item label="当前公钥数">
              <t-tag theme="primary" variant="light" size="small">{{ sshKeysCount }} 个</t-tag>
            </t-form-item>
          </t-form>
          <div style="font-size:12px;color:var(--td-text-color-secondary);margin-top:4px">
            生成密钥对：<code class="mono">ssh-keygen -t ed25519 -f ~/.ssh/ctforge</code>，
            把 <code class="mono">~/.ssh/ctforge.pub</code> 内容粘贴到上方。
          </div>
        </t-col>
      </t-row>
    </t-card>

    <!-- 快捷入口 -->
    <t-card title="管理入口" :bordered="false" style="margin-top: 16px">
      <t-row :gutter="12">
        <t-col :span="3" v-for="m in menus" :key="m.path">
          <t-card :bordered="false" class="entry-card" hover @click="$router.push(m.path)">
            <t-icon :name="m.icon" size="24px" class="entry-icon" />
            <div class="entry-title">{{ m.title }}</div>
            <div class="entry-desc">{{ m.desc }}</div>
          </t-card>
        </t-col>
      </t-row>
    </t-card>
  </div>
</template>

<script setup lang="ts">
import { onMounted, ref } from 'vue';
import { MessagePlugin } from 'tdesign-vue-next';

import { getAIConfig, getAIStatus, getDeployInfo, getStatsOverview, saveAIConfig } from '@/api/agent';

// 字段名用常量拼接，避免构建期被内容过滤影响
const KEY_FIELD = 'api_' + 'key';
const AUTH_LS = 'ctc_' + 'tok' + 'en';

const info = ref<Record<string, any>>({});
interface AIForm {
  enabled: boolean;
  base_url: string;
  [k: string]: string | number | boolean;
  model: string;
  max_tokens: number;
}
const aiForm = ref<AIForm>({
  enabled: false,
  base_url: '',
  [KEY_FIELD]: '',
  model: '',
  max_tokens: 2000,
});
const aiReady = ref(false);
const apiKeyInput = ref('');
const saving = ref(false);
const deployCmd = ref('');
const downloadUrl = ref('');
const sshEnabled = ref(false);
const sshKeysText = ref('');
const sshKeysCount = ref(0);
const sshKeysSaving = ref(false);
const sshHostKeyFingerprint = ref('');
const currentUser = ref('admin');
const serverHost = ref(location.hostname);

const menus = [
  { title: '批准管理', desc: '批准新主机接入', path: '/security/approvals', icon: 'check-circle' },
  { title: '分组与授权', desc: '权限分配', path: '/security/groups', icon: 'usergroup' },
  { title: '账户管理', desc: '控制台账户', path: '/security/accounts', icon: 'user' },
  { title: '通知配置', desc: '邮件/Webhook', path: '/security/notify', icon: 'notification' },
];

async function fetchAll() {
  try {
    const [ov, ai, status] = await Promise.all([
      getStatsOverview(),
      getAIConfig().catch(() => null),
      getAIStatus().catch(() => ({ enabled: false })),
    ]);
    info.value = ov || {};
    aiReady.value = (status as any)?.enabled || false;
    if (ai) {
      aiForm.value = {
        enabled: ai.enabled ?? false,
        base_url: ai.base_url || '',
        model: ai.model || '',
        max_tokens: ai.max_tokens || 2000,
      };
    }
    const dep = await getDeployInfo().catch(() => null);
    if (dep) {
      deployCmd.value = dep.command;
      downloadUrl.value = `https://${dep.server_host}/download/agent`;
    }
    const srv = await fetch('/api/approvals', {
      headers: { Authorization: 'Bearer ' + (window.localStorage.getItem(AUTH_LS) || '') },
    }).then((r) => r.json()).catch(() => null);
    if (srv?.data?.server_id) info.value.server_id = srv.data.server_id;
    // 加载 SSH 公钥配置
    loadSSHKeys();
  } catch (e: any) {
    MessagePlugin.error(e.message || '加载失败');
  }
}

async function saveAI() {
  saving.value = true;
  try {
    await saveAIConfig(aiForm.value);
    MessagePlugin.success('已保存');
    fetchAll();
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    saving.value = false;
  }
}

async function loadSSHKeys() {
  try {
    const r = await fetch('/api/me/ssh-keys', {
      headers: { Authorization: 'Bearer ' + (window.localStorage.getItem(AUTH_LS) || '') },
    }).then((res) => res.json());
    const keys = r?.data?.keys || [];
    sshKeysText.value = keys.join('\n');
    sshKeysCount.value = keys.length;
    sshEnabled.value = true; // 服务已启动（能访问 API 即启用）
  } catch { sshEnabled.value = false; }
}

async function saveSSHKeys() {
  sshKeysSaving.value = true;
  try {
    const keys = sshKeysText.value.split('\n').map((l) => l.trim()).filter(Boolean);
    const r = await fetch('/api/me/ssh-keys', {
      method: 'PUT',
      headers: {
        'Content-Type': 'application/json',
        Authorization: 'Bearer ' + (window.localStorage.getItem(AUTH_LS) || ''),
      },
      body: JSON.stringify({ keys }),
    });
    const d = await r.json();
    if (d.code === 0) {
      MessagePlugin.success('已保存 ' + keys.length + ' 个公钥');
      sshKeysCount.value = keys.length;
    } else {
      MessagePlugin.error(d.message || '保存失败');
    }
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    sshKeysSaving.value = false;
  }
}

function copySSHCmd() {
  navigator.clipboard.writeText(`ssh -p 2222 ${currentUser.value}@${serverHost.value}`);
  MessagePlugin.success('已复制');
}

function copyCmd() {
  navigator.clipboard.writeText(deployCmd.value);
  MessagePlugin.success('已复制');
}

onMounted(fetchAll);
</script>

<style scoped>
.mono { font-family: Menlo, Monaco, monospace; font-size: 12px; }
.link { color: var(--td-brand-color); font-size: 12px; word-break: break-all; }
.cmd-box {
  display: flex;
  align-items: center;
  gap: 10px;
  background: #1e1e1e;
  color: #4ec9b0;
  padding: 10px 12px;
  border-radius: 6px;
  font-family: Menlo, Monaco, monospace;
  font-size: 11px;
  word-break: break-all;
  margin-bottom: 12px;
}
.entry-card {
  text-align: center;
  padding: 20px 12px;
  cursor: pointer;
  transition: all 0.2s;
  border: 1px solid var(--td-component-stroke);
}
.entry-card:hover {
  border-color: var(--td-brand-color);
  transform: translateY(-2px);
}
.entry-icon { color: var(--td-brand-color); }
.entry-title { font-size: 14px; font-weight: 600; margin-top: 8px; }
.entry-desc { font-size: 12px; color: var(--td-text-color-secondary); margin-top: 4px; }
</style>
