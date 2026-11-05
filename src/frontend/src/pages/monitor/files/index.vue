<template>
  <t-card :bordered="false">
    <template #title>
      <t-space align="center">
        <span>文件管理</span>
        <t-select
          v-model="currentAgent"
          :options="agentOptions"
          placeholder="选择主机"
          style="width: 260px"
          :loading="loadingAgents"
          @change="refresh"
        />
      </t-space>
    </template>
    <template #actions>
      <t-space>
        <t-button theme="primary" @click="refresh" :loading="loading">刷新</t-button>
        <t-button theme="default" @click="goUp" :disabled="path === '/'">上级</t-button>
        <t-button theme="default" @click="showNewFile = true">新建文件</t-button>
        <t-button theme="default" @click="showMkdir = true">新建目录</t-button>
        <t-button theme="default" @click="showUpload = true">上传文件</t-button>
        <t-button-group size="small" variant="outline">
          <t-button :theme="viewMode === 'list' ? 'primary' : 'default'" @click="setView('list')">
            <t-icon name="view-list" /> 列表
          </t-button>
          <t-button :theme="viewMode === 'icon' ? 'primary' : 'default'" @click="setView('icon')">
            <t-icon name="view-module" /> 图标
          </t-button>
        </t-button-group>
      </t-space>
    </template>

    <t-breadcrumb style="margin-bottom: 12px">
      <t-breadcrumb-item @click="goTo('/')">/</t-breadcrumb-item>
      <t-breadcrumb-item v-for="(seg, i) in pathSegments" :key="i" @click="goTo(seg.path)">
        {{ seg.name }}
      </t-breadcrumb-item>
    </t-breadcrumb>

    <t-table v-if="viewMode === 'list'" :data="files" :columns="columns" :loading="loading" row-key="path" hover>
      <template #name="{ row }">
        <t-link theme="primary" hover="color" @click="onEntry(row)">
          <t-space :size="4">
            <t-icon :name="row.is_dir ? 'folder' : 'file'" />
            <span>{{ row.name }}</span>
          </t-space>
        </t-link>
      </template>
      <template #size="{ row }">
        {{ row.is_dir ? '-' : formatSize(row.size) }}
      </template>
      <template #mod="{ row }">
        {{ formatTime(row.mod_time) }}
      </template>
      <template #op="{ row }">
        <t-space break-line>
          <t-link v-if="!row.is_dir" theme="primary" hover="color" @click="onEditFile(row)">编辑</t-link>
          <t-link v-if="!row.is_dir" theme="primary" hover="color" @click="download(row)">下载</t-link>
          <t-link theme="primary" hover="color" @click="onRename(row)">重命名</t-link>
          <t-link theme="danger" hover="color" @click="onDelete(row)">删除</t-link>
        </t-space>
      </template>
    </t-table>

    <!-- 图标视图（类 Windows 资源管理器） -->
    <div v-else class="icon-view" :class="{ loading }">
      <t-loading :loading="loading" size="small">
        <div class="icon-grid">
          <div
            v-for="row in files"
            :key="row.path"
            class="icon-item"
            :class="{ 'is-dir': row.is_dir }"
            @dblclick="onEntry(row)"
            @click="selectedPath = row.path"
            @contextmenu.prevent="openContextMenu($event, row)"
            :title="row.name"
          >
            <div class="icon-thumb">
              <t-icon :name="fileIcon(row)" size="42px" :class="iconClass(row)" />
            </div>
            <div class="icon-name">{{ row.name }}</div>
            <div class="icon-meta">{{ row.is_dir ? '文件夹' : formatSize(row.size) }}</div>
            <div class="icon-actions" @click.stop>
              <t-tooltip content="打开/编辑">
                <t-icon name="edit" @click="onEntry(row)" />
              </t-tooltip>
              <t-tooltip v-if="!row.is_dir" content="下载">
                <t-icon name="download" @click="download(row)" />
              </t-tooltip>
              <t-tooltip content="重命名">
                <t-icon name="edit-1" @click="onRename(row)" />
              </t-tooltip>
              <t-tooltip content="删除">
                <t-icon name="delete" class="danger" @click="onDelete(row)" />
              </t-tooltip>
            </div>
          </div>
        </div>
        <t-empty v-if="!loading && files.length === 0" description="空目录" />
      </t-loading>
    </div>

    <!-- 图标右键菜单 -->
    <div
      v-if="ctxMenu.show"
      class="ctx-menu"
      :style="{ left: ctxMenu.x + 'px', top: ctxMenu.y + 'px' }"
      @click.stop
    >
      <div class="ctx-header">{{ ctxMenu.target?.name }}</div>
      <div class="ctx-item" @click="ctxOpen">
        <t-icon :name="ctxMenu.target?.is_dir ? 'folder-open' : 'edit'" />
        <span>{{ ctxMenu.target?.is_dir ? '打开' : '编辑' }}</span>
      </div>
      <div v-if="!ctxMenu.target?.is_dir" class="ctx-item" @click="ctxDownload">
        <t-icon name="download" />
        <span>下载</span>
      </div>
      <div class="ctx-divider"></div>
      <div class="ctx-item" @click="ctxRename">
        <t-icon name="edit-1" />
        <span>重命名</span>
      </div>
      <div class="ctx-item" @click="ctxMove">
        <t-icon name="move" />
        <span>移动到…</span>
      </div>
      <div v-if="!ctxMenu.target?.is_dir" class="ctx-item" @click="ctxCopyPath">
        <t-icon name="link" />
        <span>复制路径</span>
      </div>
      <div class="ctx-divider"></div>
      <div class="ctx-item" @click="ctxProps">
        <t-icon name="info-circle" />
        <span>属性</span>
      </div>
      <div class="ctx-divider"></div>
      <div class="ctx-item danger" @click="ctxDelete">
        <t-icon name="delete" />
        <span>删除</span>
      </div>
    </div>

    <!-- 移动到目录 -->
    <t-dialog v-model:visible="showMove" header="移动到" :on-confirm="doMove" :confirm-btn="{ loading: moving }">
      <t-form :data="moveForm" label-width="80px">
        <t-form-item label="目标目录">
          <t-input v-model="moveForm.dest" placeholder="如 /opt/backup" />
        </t-form-item>
      </t-form>
      <t-alert theme="info" style="font-size: 12px">
        将把 <b>{{ moveForm.src }}</b> 移动到 <b>{{ moveForm.dest }}/</b>
      </t-alert>
    </t-dialog>

    <!-- 文件属性 -->
    <t-dialog v-model:visible="showProps" header="属性" width="560px" :footer="false">
      <t-descriptions v-if="propTarget" :column="1" size="small" bordered>
        <t-descriptions-item label="名称">{{ propTarget.name }}</t-descriptions-item>
        <t-descriptions-item label="完整路径">
          <span class="mono">{{ propTarget.path }}</span>
        </t-descriptions-item>
        <t-descriptions-item label="类型">{{ propTarget.is_dir ? '目录' : '文件' }}</t-descriptions-item>
        <t-descriptions-item label="大小">{{ propTarget.is_dir ? '-' : formatSize(propTarget.size) }}</t-descriptions-item>
        <t-descriptions-item label="权限">
          <span class="mono">{{ propTarget.mode }}</span>
          （{{ propTarget.mode.slice(1) }}）
        </t-descriptions-item>
        <t-descriptions-item label="修改时间">{{ formatTime(propTarget.mod_time) }}</t-descriptions-item>
      </t-descriptions>
    </t-dialog>

    <!-- 新建文本文件 -->
    <t-dialog v-model:visible="showNewFile" header="新建文本文件" :on-confirm="doNewFile" :confirm-btn="{ loading: creating }">
      <t-form :data="newFileForm" label-width="80px">
        <t-form-item label="文件名">
          <t-input v-model="newFileForm.name" placeholder="如 readme.md / config.yaml" />
        </t-form-item>
        <t-form-item label="初始内容">
          <t-textarea v-model="newFileForm.content" placeholder="可留空" :autosize="{ minRows: 6 }" />
        </t-form-item>
      </t-form>
    </t-dialog>

    <t-dialog v-model:visible="showMkdir" header="新建目录" :on-confirm="doMkdir">
      <t-input v-model="mkdirName" placeholder="目录名" />
    </t-dialog>

    <t-dialog v-model:visible="showUpload" header="上传文件" :on-confirm="doUpload" :confirm-btn="{ loading: uploading }">
      <t-progress v-if="uploading" :percentage="uploadProgress" style="margin-bottom: 12px" />
      <t-input v-model="uploadName" placeholder="远端文件名" style="margin-bottom: 8px" />
      <input ref="fileInput" type="file" @change="onFilePicked" />
    </t-dialog>

    <!-- 在线编辑器 -->
    <t-dialog v-model:visible="showEditor" :header="`编辑：${editPath}`" width="900px" :on-confirm="doSaveFile" :confirm-btn="{ loading: editSaving, disabled: editBinary || editTruncated }">
      <t-alert v-if="editBinary" theme="warning" style="margin-bottom: 12px">
        检测到二进制文件，无法在线编辑。请使用下载功能。
      </t-alert>
      <t-alert v-else-if="editTruncated" theme="warning" style="margin-bottom: 12px">
        文件超过 8MB，编辑器仅加载前 8MB。大文件建议下载后用本地编辑器修改。
      </t-alert>
      <t-loading :loading="editLoading">
        <div v-if="!editBinary" class="editor-wrap">
          <div class="editor-toolbar">
            <t-radio-group v-model="editMode" variant="default-filled" size="small">
              <t-radio-button value="view">查看</t-radio-button>
              <t-radio-button value="edit">编辑</t-radio-button>
            </t-radio-group>
            <t-checkbox v-if="isMarkdownFile" v-model="editMarkdown">Markdown 预览</t-checkbox>
            <span class="path-hint">{{ editPath }}</span>
            <t-button
              v-if="editMode === 'edit'"
              size="small"
              theme="default"
              variant="outline"
              @click="formatJson"
            >JSON 美化</t-button>
            <t-checkbox
              v-if="editMode === 'edit' && editContent !== originalContent"
              v-model="showDiff"
            >显示修改差异</t-checkbox>
            <span class="stat-hint">{{ editContent.length }} 字符 · {{ lineCount }} 行</span>
            <t-tag v-if="editTruncated" theme="warning" variant="light" size="small">已截断</t-tag>
          </div>
          <!-- 单一容器：CodeMirror 始终挂在这里，用 CSS 切换布局，避免 DOM 重建导致空白 -->
          <div class="editor-body" :class="{ 'md-active': showMdPreview }">
            <div class="code-area" ref="editorEl"></div>
            <div class="md-preview markdown-body" v-show="showMdPreview" v-html="mdHtml"></div>
          </div>
          <!-- 修改差异预览 -->
          <div v-if="showDiff && editMode === 'edit'" class="diff-panel">
            <div class="diff-head">
              <span>修改差异（{{ diffLines.length }} 行变化）</span>
              <t-icon name="close" @click="showDiff = false" />
            </div>
            <pre class="diff-body">{{ diffText }}</pre>
          </div>
        </div>
      </t-loading>
    </t-dialog>

    <t-dialog v-model:visible="showRename" header="重命名" :on-confirm="doRename">
      <t-input v-model="renameTo" placeholder="新名称" />
    </t-dialog>
  </t-card>
</template>

<script setup lang="ts">
import { computed, nextTick, onMounted, onUnmounted, ref, watch } from 'vue';
// 直接静态 import @codemirror/*：确保全应用唯一实例
import { EditorView, keymap, lineNumbers, highlightActiveLineGutter, EditorView as EV } from '@codemirror/view';
import { EditorState } from '@codemirror/state';
import { oneDark } from '@codemirror/theme-one-dark';
import { markdown } from '@codemirror/lang-markdown';
import { defaultKeymap, history, historyKeymap } from '@codemirror/commands';
import { syntaxHighlighting, defaultHighlightStyle, bracketMatching } from '@codemirror/language';
import { marked } from 'marked';
import DOMPurify from 'dompurify';
import { useRoute } from 'vue-router';
import { MessagePlugin, DialogPlugin } from 'tdesign-vue-next';
import type { PrimaryTableCol } from 'tdesign-vue-next';
import dayjs from 'dayjs';

import { fileAction, listAgents, listFiles, readFile, writeFile, readFileText, saveFileText, type FileEntry } from '@/api/agent';

const route = useRoute();
const currentAgent = ref<string>((route.query.agent as string) || '');
const agents = ref<{ label: string; value: string }[]>([]);
const loadingAgents = ref(false);

const path = ref('/');
const files = ref<FileEntry[]>([]);
const loading = ref(false);
const viewMode = ref<'list' | 'icon'>(localStorage.getItem('ctc_file_view') === 'icon' ? 'icon' : 'list');
const selectedPath = ref('');
const showNewFile = ref(false);
/* 右键菜单状态 */
const ctxMenu = ref({
  show: false,
  x: 0,
  y: 0,
  target: null as FileEntry | null,
});
const showMove = ref(false);
const moving = ref(false);
const moveForm = ref({ src: '', dest: '' });
const showProps = ref(false);
const propTarget = ref<FileEntry | null>(null);
const creating = ref(false);
const newFileForm = ref({ name: '', content: '' });

const showMkdir = ref(false);
const mkdirName = ref('');
const showUpload = ref(false);
const uploadName = ref('');
const uploading = ref(false);
const uploadProgress = ref(0);
const fileInput = ref<HTMLInputElement>();
let pickedFile: File | null = null;
const showRename = ref(false);
const renameTo = ref('');
let renameTarget: FileEntry | null = null;

/* 在线编辑 */
const showEditor = ref(false);
const editLoading = ref(false);
const editSaving = ref(false);
const editPath = ref('');
const editContent = ref('');
const editBinary = ref(false);
const editTruncated = ref(false);
const editMarkdown = ref(false);
const editMode = ref<'view' | 'edit'>('view'); // 默认查看模式
const originalContent = ref('');   // 打开时的原始内容（用于差异对比）
const showDiff = ref(false);
const editorEl = ref<HTMLElement>();
let cmView: any = null;

const lineCount = computed(() => (editContent.value ? editContent.value.split('\n').length : 0));
const isMarkdownFile = computed(() => /\.(md|markdown|mdx)$/i.test(editPath.value));

// Markdown 预览：仅在 markdown 文件且勾选时显示
const showMdPreview = computed(() => editMarkdown.value && isMarkdownFile.value);

// 编辑模式切换需要重建 CodeMirror（只读状态在创建时确定）
watch(editMode, async (m) => {
  if (!showEditor.value || editBinary.value) return;
  await nextTick();
  initEditor();
});

// Markdown 预览开关：切换布局后重新测量
watch(showMdPreview, () => {
  setTimeout(() => cmView?.requestMeasure?.(), 80);
});

const mdHtml = computed(() => {
  if (!showMdPreview.value) return '';
  const raw = marked.parse(editContent.value || '', { breaks: true, gfm: true }) as string;
  return DOMPurify.sanitize(raw);
});

const agentOptions = computed(() => agents.value);

const columns: PrimaryTableCol[] = [
  { colKey: 'name', title: '名称', ellipsis: true },
  { colKey: 'size', title: '大小', width: 110 },
  { colKey: 'mode', title: '权限', width: 110 },
  { colKey: 'mod', title: '修改时间', width: 160 },
  { colKey: 'op', title: '操作', width: 150, fixed: 'right' },
];

const pathSegments = computed(() => {
  const segs = path.value.split('/').filter(Boolean);
  let cur = '';
  return segs.map((s) => {
    cur += '/' + s;
    return { name: s, path: cur };
  });
});

function formatSize(n: number) {
  if (n > 1024 * 1024 * 1024) return (n / 1024 / 1024 / 1024).toFixed(1) + 'G';
  if (n > 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + 'M';
  if (n > 1024) return (n / 1024).toFixed(1) + 'K';
  return n + 'B';
}

function formatTime(t: number) {
  return t ? dayjs(t * 1000).format('YYYY-MM-DD HH:mm') : '-';
}

function goTo(p: string) {
  path.value = p;
  refresh();
}

function goUp() {
  const segs = path.value.split('/').filter(Boolean);
  segs.pop();
  goTo('/' + segs.join('/'));
}

function join(dir: string, name: string) {
  return dir === '/' ? '/' + name : dir.replace(/\/$/, '') + '/' + name;
}

/** 右键菜单 */
function openContextMenu(e: MouseEvent, row: FileEntry) {
  selectedPath.value = row.path;
  ctxMenu.value = {
    show: true,
    x: e.clientX,
    y: e.clientY,
    target: row,
  };
}

function closeContextMenu() {
  ctxMenu.value.show = false;
}

function ctxOpen() {
  if (ctxMenu.value.target) onEntry(ctxMenu.value.target);
  closeContextMenu();
}

function ctxDownload() {
  if (ctxMenu.value.target && !ctxMenu.value.target.is_dir) download(ctxMenu.value.target);
  closeContextMenu();
}

function ctxRename() {
  if (ctxMenu.value.target) onRename(ctxMenu.value.target);
  closeContextMenu();
}

function ctxDelete() {
  if (ctxMenu.value.target) onDelete(ctxMenu.value.target);
  closeContextMenu();
}

function ctxMove() {
  const t = ctxMenu.value.target;
  if (!t) return;
  moveForm.value = {
    src: t.path,
    dest: path.value,
  };
  showMove.value = true;
  closeContextMenu();
}

async function doMove() {
  const src = moveForm.value.src;
  const dest = moveForm.value.dest.trim();
  if (!dest) {
    MessagePlugin.warning('请输入目标目录');
    return;
  }
  const name = src.split('/').pop() || '';
  const to = dest.endsWith('/') ? dest + name : dest + '/' + name;
  moving.value = true;
  try {
    await fileAction(currentAgent.value, 'rename', '', src, to);
    MessagePlugin.success('已移动');
    showMove.value = false;
    refresh();
  } catch (e: any) {
    MessagePlugin.error(e.message || '移动失败');
  } finally {
    moving.value = false;
  }
}

function ctxCopyPath() {
  const t = ctxMenu.value.target;
  if (!t) return;
  const path = t.path;
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(path).then(() => {
      MessagePlugin.success('已复制路径');
    }).catch(() => fallbackCopy(path));
  } else {
    fallbackCopy(path);
  }
  closeContextMenu();
}

function fallbackCopy(text: string) {
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.cssText = 'position:fixed;left:-9999px;top:0;opacity:0;';
  document.body.appendChild(ta);
  ta.select();
  try {
    document.execCommand('copy');
    MessagePlugin.success('已复制路径');
  } catch {
    MessagePlugin.info('路径：' + text);
  }
  document.body.removeChild(ta);
}

function ctxProps() {
  const t = ctxMenu.value.target;
  if (!t) return;
  propTarget.value = t;
  showProps.value = true;
  closeContextMenu();
}

/** 切换视图模式（记忆到本地） */
function setView(m: 'list' | 'icon') {
  viewMode.value = m;
  localStorage.setItem('ctc_file_view', m);
}

/** 根据扩展名返回图标 */
function fileIcon(row: FileEntry): string {
  if (row.is_dir) return 'folder';
  const ext = row.name.split('.').pop()?.toLowerCase() || '';
  const map: Record<string, string> = {
    /* 文档 */
    md: 'file-word', markdown: 'file-word', txt: 'file', log: 'file',
    doc: 'file-word', docx: 'file-word', rtf: 'file-word', odt: 'file-word',
    pdf: 'file-pdf',
    /* 代码 */
    js: 'file-code', jsx: 'file-code', ts: 'file-code', tsx: 'file-code',
    py: 'file-code', go: 'file-code', c: 'file-code', h: 'file-code',
    cpp: 'file-code', cc: 'file-code', cxx: 'file-code', hpp: 'file-code',
    java: 'file-code', kt: 'file-code', swift: 'file-code', rs: 'file-code',
    rb: 'file-code', php: 'file-code', cs: 'file-code', scala: 'file-code',
    lua: 'file-code', pl: 'file-code', pm: 'file-code', r: 'file-code',
    sh: 'file-code', bash: 'file-code', zsh: 'file-code', fish: 'file-code',
    ps1: 'file-code', bat: 'file-code', cmd: 'file-code',
    sql: 'file-code', graphql: 'file-code', proto: 'file-code', thrift: 'file-code',
    html: 'file-code', htm: 'file-code', css: 'file-code', scss: 'file-code', less: 'file-code',
    vue: 'file-code', svelte: 'file-code', astro: 'file-code', nunjucks: 'file-code',
    json: 'file-code', json5: 'file-code', jsonc: 'file-code',
    yaml: 'file-code', yml: 'file-code', toml: 'file-code', xml: 'file-code', ini: 'file-code', cfg: 'file-code',
    /* 图片 */
    png: 'image', jpg: 'image', jpeg: 'image', gif: 'image', svg: 'image', webp: 'image',
    bmp: 'image', ico: 'image', tiff: 'image', tif: 'image', avif: 'image', heic: 'image',
    /* 压缩包 */
    zip: 'file-zip', tar: 'file-zip', gz: 'file-zip', tgz: 'file-zip', xz: 'file-zip', '7z': 'file-zip',
    rar: 'file-zip', bz2: 'file-zip', zst: 'file-zip', deb: 'file-zip', rpm: 'file-zip', apk: 'file-zip',
    /* 办公 */
    xls: 'file-excel', xlsx: 'file-excel', csv: 'file-excel', ods: 'file-excel', numbers: 'file-excel',
    ppt: 'file-ppt', pptx: 'file-ppt', odp: 'file-ppt', key: 'file-ppt',
    /* 音视频 */
    mp4: 'video', mkv: 'video', avi: 'video', mov: 'video', wmv: 'video', flv: 'video',
    webm: 'video', m4v: 'video', mpg: 'video', mpeg: 'video', m2ts: 'video', mts: 'video',
    mp3: 'sound', wav: 'sound', flac: 'sound', ogg: 'sound', m4a: 'sound', aac: 'sound', wma: 'sound',
    /* 二进制 / 其他 */
    bin: 'file-unknown', so: 'file-unknown', exe: 'file-unknown', dll: 'file-unknown', dylib: 'file-unknown',
    elf: 'file-unknown', o: 'file-unknown', obj: 'file-unknown', class: 'file-unknown', pyc: 'file-unknown',
    /* 系统配置 */
    service: 'setting', timer: 'setting', mount: 'setting', socket: 'setting',
    conf: 'setting', config: 'setting', env: 'setting', systemd: 'setting',
    /* 网络 */
    hosts: 'internet', resolv: 'internet', netplan: 'internet',
    /* Docker */
    dockerfile: 'layers', 'docker-compose': 'layers', 'docker-compose.yml': 'layers',
  };
  return map[ext] || 'file';
}

function iconClass(row: FileEntry) {
  if (row.is_dir) return 'ic-dir';
  const ext = row.name.split('.').pop()?.toLowerCase() || '';
  if (['md', 'txt', 'log'].includes(ext)) return 'ic-text';
  if (['js', 'ts', 'py', 'go', 'c', 'h', 'java', 'sh', 'json', 'yaml', 'yml', 'xml'].includes(ext)) return 'ic-code';
  if (['png', 'jpg', 'jpeg', 'gif', 'svg', 'webp'].includes(ext)) return 'ic-img';
  if (['zip', 'tar', 'gz', 'tgz', 'xz', '7z', 'rar', 'bz2', 'zst', 'deb', 'rpm', 'apk'].includes(ext)) return 'ic-zip';
  if (['xls', 'xlsx', 'csv', 'ods', 'numbers'].includes(ext)) return 'ic-excel';
  if (['ppt', 'pptx', 'odp', 'key'].includes(ext)) return 'ic-ppt';
  if (['mp4', 'mkv', 'avi', 'mov', 'wmv', 'flv', 'webm'].includes(ext)) return 'ic-video';
  if (['mp3', 'wav', 'flac', 'ogg', 'm4a', 'aac'].includes(ext)) return 'ic-sound';
  if (['service', 'timer', 'mount', 'socket', 'conf', 'config', 'env', 'systemd'].includes(ext)) return 'ic-config';
  if (['dockerfile', 'docker-compose'].includes(ext)) return 'ic-docker';
  return 'ic-bin';
}

/** 新建文本文件 */
async function doNewFile() {
  const name = newFileForm.value.name.trim();
  if (!name) {
    MessagePlugin.warning('请输入文件名');
    return;
  }
  creating.value = true;
  try {
    const b64 = btoa(unescape(encodeURIComponent(newFileForm.value.content || '')));
    await writeFile(currentAgent.value, join(path.value, name), b64);
    MessagePlugin.success('文件已创建');
    showNewFile.value = false;
    newFileForm.value = { name: '', content: '' };
    refresh();
    // 创建后直接打开编辑
    const created = files.value.find((f) => f.name === name);
    if (created) setTimeout(() => onEditFile(created), 300);
  } catch (e: any) {
    MessagePlugin.error(e.message || '创建失败');
  } finally {
    creating.value = false;
  }
}

async function refresh() {
  if (!currentAgent.value) return;
  loading.value = true;
  try {
    const r = await listFiles(currentAgent.value, path.value);
    files.value = [...r.files].sort((a, b) => Number(b.is_dir) - Number(a.is_dir) || a.name.localeCompare(b.name));
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取目录失败');
  } finally {
    loading.value = false;
  }
}

function onEntry(row: FileEntry) {
  if (row.is_dir) goTo(row.path);
  else onEditFile(row);  // 默认触发编辑，二进制在编辑层询问下载
}

async function download(row: FileEntry) {
  // 直接让浏览器请求流式下载接口，服务端分块从 agent 拉取并边收边写
  const token = (window.localStorage.getItem('ctc_' + 'tok' + 'en') || '');
  const url = `/api/agents/${encodeURIComponent(currentAgent.value)}/files?path=${encodeURIComponent(row.path)}&download=1`;
  try {
    MessagePlugin.info('开始下载，大文件可能需要一些时间…');
    const resp = await fetch(url, { headers: { Authorization: 'Bearer ' + token } });
    if (!resp.ok) {
      const err = await resp.json().catch(() => ({ message: `HTTP ${resp.status}` }));
      throw new Error(err.message || `HTTP ${resp.status}`);
    }
    const blob = await resp.blob();
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = row.name;
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(() => URL.revokeObjectURL(a.href), 30000);
    const size = resp.headers.get('X-File-Size');
    MessagePlugin.success(`下载完成${size ? '（' + formatSize(Number(size)) + '）' : ''}`);
  } catch (e: any) {
    MessagePlugin.error(e.message || '下载失败');
  }
}

async function doMkdir() {
  if (!mkdirName.value) return;
  try {
    await fileAction(currentAgent.value, 'mkdir', join(path.value, mkdirName.value));
    MessagePlugin.success('创建成功');
    showMkdir.value = false;
    mkdirName.value = '';
    refresh();
  } catch (e: any) {
    MessagePlugin.error(e.message || '创建失败');
  }
}

function onFilePicked(e: Event) {
  pickedFile = (e.target as HTMLInputElement).files?.[0] ?? null;
  if (pickedFile && !uploadName.value) uploadName.value = pickedFile.name;
}

async function doUpload() {
  if (!pickedFile || !uploadName.value) {
    MessagePlugin.warning('请选择文件并填写远端文件名');
    return;
  }
  uploading.value = true;
  uploadProgress.value = 0;
  try {
    const target = join(path.value, uploadName.value);
    const CHUNK = 3 * 1024 * 1024;
    const total = pickedFile.size;
    let offset = 0;
    let first = true;

    while (offset < total || (total === 0 && first)) {
      const slice = pickedFile.slice(offset, Math.min(offset + CHUNK, total));
      const buf = await slice.arrayBuffer();
      let bin = '';
      const bytes = new Uint8Array(buf);
      const STEP = 8192;
      for (let i = 0; i < bytes.length; i += STEP) {
        bin += String.fromCharCode.apply(null, Array.from(bytes.subarray(i, i + STEP)) as any);
      }
      const b64 = btoa(bin);

      await writeFile(currentAgent.value, target, b64, !first);
      offset += buf.byteLength;
      first = false;
      uploadProgress.value = total > 0 ? Math.min(100, Math.round((offset / total) * 100)) : 100;
      if (buf.byteLength === 0) break;
    }

    MessagePlugin.success('上传完成');
    showUpload.value = false;
    uploadName.value = '';
    pickedFile = null;
    uploadProgress.value = 0;
    if (fileInput.value) fileInput.value.value = '';
    refresh();
  } catch (e: any) {
    MessagePlugin.error(e.message || '上传失败');
  } finally {
    uploading.value = false;
  }
}

function onRename(row: FileEntry) {
  renameTarget = row;
  renameTo.value = row.name;
  showRename.value = true;
}

async function doRename() {
  if (!renameTarget || !renameTo.value) return;
  try {
    await fileAction(currentAgent.value, 'rename', '', renameTarget.path, join(path.value, renameTo.value));
    MessagePlugin.success('重命名成功');
    showRename.value = false;
    refresh();
  } catch (e: any) {
    MessagePlugin.error(e.message || '重命名失败');
  }
}

function onDelete(row: FileEntry) {
  const dlg = DialogPlugin.confirm({
    header: '确认删除',
    body: `确定删除 ${row.path} 吗？`,
    theme: 'danger',
    onConfirm: async () => {
      try {
        await fileAction(currentAgent.value, 'delete', row.path);
        MessagePlugin.success('已删除');
        refresh();
      } catch (e: any) {
        MessagePlugin.error(e.message || '删除失败');
      }
      dlg.destroy();
    },
  });
}

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

async function onEditFile(row: FileEntry) {
  editLoading.value = true;
  showEditor.value = true;
  editPath.value = row.path;
  editContent.value = '';
  editBinary.value = false;
  editTruncated.value = false;
  editMode.value = 'view';
  // md 文件默认勾选预览
  editMarkdown.value = /\.(md|markdown|mdx)$/i.test(row.name);
  try {
    const r = await readFileText(currentAgent.value, row.path);
    if (r.binary) {
      editBinary.value = true;
      const dlg = DialogPlugin.confirm({
        header: '二进制文件',
        body: `${row.name} 是二进制文件，无法在线编辑。是否下载？`,
        confirmBtn: '下载', cancelBtn: '取消',
        onConfirm: async () => { dlg.destroy(); await download(row); },
        onCancel: () => { dlg.destroy(); showEditor.value = false; },
      });
    } else {
      editContent.value = r.content;
      originalContent.value = r.content;
      editTruncated.value = r.truncated;
      showDiff.value = false;
      // 等 dialog 打开动画 + DOM 挂载完成
      await nextTick();
      await new Promise((res) => setTimeout(res, 120));
      await nextTick();
      await initEditor();
    }
  } catch (e: any) {
    MessagePlugin.error(e.message || '读取失败');
    showEditor.value = false;
  } finally {
    editLoading.value = false;
  }
}

async function initEditor() {
  const el = editorEl.value;
  if (!el) return;
  destroyEditor();
  try {
    const exts: any[] = [
      lineNumbers(),
      highlightActiveLineGutter(),
      bracketMatching(),
      history(),
      syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
      keymap.of([...defaultKeymap, ...historyKeymap] as any),
      oneDark,
      EditorView.updateListener.of((u: any) => {
        if (u.docChanged) editContent.value = u.state.doc.toString();
      }),
    ];
    if (editMarkdown.value) exts.push(markdown());
    if (editMode.value === 'view') exts.push(EditorState.readOnly.of(true), EditorView.editable.of(false));
    const state = EditorState.create({ doc: editContent.value, extensions: exts });
    cmView = new EditorView({ state, parent: el });
  } catch (e: any) {
    console.error('CodeMirror init error:', e);
    MessagePlugin.error('编辑器加载失败：' + (e?.message || e));
  }
}

function destroyEditor() {
  if (cmView) { cmView.destroy(); cmView = null; }
  if (editorEl.value) editorEl.value.innerHTML = '';
}

/** 简单行级 diff：找出新增/删除/修改的行 */
const diffLines = computed(() => {
  const a = originalContent.value.split('\n');
  const b = editContent.value.split('\n');
  const out: string[] = [];
  const max = Math.max(a.length, b.length);
  for (let i = 0; i < max; i++) {
    const la = a[i];
    const lb = b[i];
    if (la === lb) continue;
    if (la !== undefined && lb !== undefined) {
      out.push(`- ${la}`);
      out.push(`+ ${lb}`);
    } else if (la !== undefined) {
      out.push(`- ${la}`);
    } else if (lb !== undefined) {
      out.push(`+ ${lb}`);
    }
  }
  return out;
});

const diffText = computed(() => diffLines.value.join('\n') || '(无变化)');

/** JSON 美化：解析后 2 空格缩进重排 */
function formatJson() {
  if (!editContent.value.trim()) return;
  try {
    const obj = JSON.parse(editContent.value);
    editContent.value = JSON.stringify(obj, null, 2);
    // 同步到 CodeMirror
    syncEditorContent(editContent.value);
    MessagePlugin.success('JSON 已格式化');
  } catch (e: any) {
    MessagePlugin.error('JSON 解析失败：' + (e.message || '格式错误'));
  }
}

/** 把内容同步回 CodeMirror 视图 */
function syncEditorContent(text: string) {
  if (!cmView) return;
  cmView.dispatch({
    changes: { from: 0, to: cmView.state.doc.length, insert: text },
  });
}

async function doSaveFile() {
  if (!editPath.value) return;
  editSaving.value = true;
  try {
    await saveFileText(currentAgent.value, editPath.value, editContent.value);
    MessagePlugin.success('已保存');
    showEditor.value = false;
  } catch (e: any) {
    MessagePlugin.error(e.message || '保存失败');
  } finally {
    editSaving.value = false;
  }
}

onMounted(async () => {
  await loadAgents();
  refresh();
  window.addEventListener('click', closeContextMenu);
});
onUnmounted(() => window.removeEventListener('click', closeContextMenu));
</script>

<style scoped>
.editor-wrap {
  border: 1px solid var(--td-component-stroke);
  border-radius: 6px;
  overflow: hidden;
}
.editor-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 6px 12px;
  background: var(--td-bg-color-container-hover);
  font-size: 12px;
  color: var(--td-text-color-secondary);
  flex-wrap: wrap;
}
.editor-toolbar .path-hint {
  flex: 1;
  font-family: monospace;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.editor-toolbar .stat-hint {
  font-family: monospace;
  color: var(--td-text-color-placeholder);
}

/* 编辑器主体：查看 / 编辑 / 预览共用容器 */
.editor-body {
  display: flex;
  min-height: 420px;
  max-height: 600px;
  overflow: auto;
  background: #1e1e1e;
}
.editor-body .code-area {
  flex: 1;
  min-width: 0;
}
.editor-body.md-active {
  display: grid;
  grid-template-columns: 1fr 1fr;
}

/* CodeMirror 在代码区的最小尺寸 */
.editor-body .code-area .cm-editor {
  min-height: 420px;
  max-height: 600px;
  font-size: 13px;
}

/* Markdown 预览面板 */
.md-preview {
  padding: 16px 20px;
  overflow: auto;
  background: #1e1e1e;
  color: #d4d4d4;
  border-left: 1px solid #333;
  max-height: 600px;
  font-size: 14px;
  line-height: 1.8;
}
.markdown-body h1,
.markdown-body h2,
.markdown-body h3 {
  color: #e8e8e8;
  border-bottom: 1px solid #3c3c3c;
  padding-bottom: 6px;
  margin-top: 20px;
  margin-bottom: 10px;
}
.markdown-body h1 { font-size: 22px; }
.markdown-body h2 { font-size: 18px; }
.markdown-body h3 { font-size: 15px; }
.markdown-body p { margin: 10px 0; }
.markdown-body a { color: #4fc3f7; text-decoration: none; }
.markdown-body a:hover { text-decoration: underline; }
.markdown-body pre {
  background: #141414;
  padding: 12px 14px;
  border-radius: 6px;
  overflow: auto;
  border: 1px solid #333;
  margin: 12px 0;
}
.markdown-body code {
  background: #2d2d2d;
  color: #ce9178;
  padding: 2px 5px;
  border-radius: 3px;
  font-size: 13px;
}
.markdown-body pre code { background: none; color: #d4d4d4; padding: 0; }
.markdown-body blockquote {
  border-left: 3px solid #4fc3f7;
  margin: 12px 0;
  padding: 4px 16px;
  color: #9e9e9e;
  background: #252526;
}
.markdown-body table {
  border-collapse: collapse;
  margin: 12px 0;
  width: 100%;
}
.markdown-body th,
.markdown-body td {
  border: 1px solid #3c3c3c;
  padding: 6px 10px;
  text-align: left;
}
.markdown-body th { background: #252526; color: #e8e8e8; }
.markdown-body ul,
.markdown-body ol { padding-left: 24px; }
.markdown-body li { margin: 4px 0; }
.markdown-body hr { border: none; border-top: 1px solid #3c3c3c; margin: 20px 0; }
.markdown-body img { max-width: 100%; }

/* ===== 图标视图（类 Windows 资源管理器）===== */
.icon-view {
  min-height: 400px;
}
.icon-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(110px, 1fr));
  gap: 8px;
  padding: 4px;
}
.icon-item {
  position: relative;
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 12px 8px 10px;
  border-radius: 6px;
  border: 1px solid transparent;
  cursor: pointer;
  transition: background 0.15s, border-color 0.15s;
  user-select: none;
}
.icon-item:hover {
  background: var(--td-bg-color-container-hover);
  border-color: var(--td-component-stroke);
}
.icon-item:active {
  background: var(--td-bg-color-container-active);
}
.icon-thumb {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 56px;
  height: 56px;
  margin-bottom: 6px;
}
.icon-name {
  font-size: 12px;
  line-height: 1.4;
  text-align: center;
  word-break: break-all;
  display: -webkit-box;
  -webkit-line-clamp: 2;
  -webkit-box-orient: vertical;
  overflow: hidden;
  max-width: 100%;
  color: var(--td-text-color-primary);
}
.icon-meta {
  font-size: 11px;
  color: var(--td-text-color-placeholder);
  margin-top: 2px;
}
.icon-actions {
  position: absolute;
  top: 4px;
  right: 4px;
  display: none;
  gap: 4px;
  background: var(--td-bg-color-container);
  border-radius: 4px;
  padding: 2px 4px;
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.12);
}
.icon-item:hover .icon-actions {
  display: flex;
}
.icon-actions .t-icon {
  font-size: 14px;
  cursor: pointer;
  color: var(--td-text-color-secondary);
}
.icon-actions .t-icon:hover {
  color: var(--td-brand-color);
}
.icon-actions .t-icon.danger:hover {
  color: var(--td-error-color);
}

/* 图标配色 */
.ic-dir { color: #f5b041; }
.ic-text { color: #5d9cec; }
.ic-code { color: #4ec9b0; }
.ic-img { color: #d16ba5; }
.ic-zip { color: #b07cc6; }
.ic-excel { color: #70ad47; }
.ic-ppt { color: #e74c3c; }
.ic-video { color: #9b59b6; }
.ic-sound { color: #1abc9c; }
.ic-config { color: #e67e22; }
.ic-docker { color: #2496ed; }
.ic-bin { color: #9e9e9e; }

/* ===== 右键菜单 ===== */
.ctx-menu {
  position: fixed;
  z-index: 3000;
  min-width: 170px;
  background: var(--td-bg-color-container);
  border: 1px solid var(--td-component-stroke);
  border-radius: 6px;
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.16);
  padding: 4px 0;
  font-size: 13px;
  user-select: none;
}
.ctx-header {
  padding: 6px 14px 8px;
  font-size: 12px;
  color: var(--td-text-color-placeholder);
  border-bottom: 1px solid var(--td-component-stroke);
  margin-bottom: 4px;
  max-width: 220px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}
.ctx-item {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 7px 14px;
  cursor: pointer;
  color: var(--td-text-color-primary);
  transition: background 0.12s;
}
.ctx-item:hover {
  background: var(--td-bg-color-container-hover);
  color: var(--td-brand-color);
}
.ctx-item.danger:hover {
  background: var(--td-error-color-1);
  color: var(--td-error-color);
}
.ctx-item .t-icon {
  font-size: 15px;
  color: var(--td-text-color-secondary);
}
.ctx-item:hover .t-icon {
  color: inherit;
}
.ctx-divider {
  height: 1px;
  background: var(--td-component-stroke);
  margin: 4px 0;
}
.mono { font-family: Menlo, Monaco, monospace; font-size: 12px; }

/* ===== 差异面板 ===== */
.diff-panel {
  border-top: 1px solid var(--td-component-stroke);
  background: #141414;
  max-height: 240px;
  display: flex;
  flex-direction: column;
}
.diff-head {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 6px 12px;
  font-size: 12px;
  font-weight: 600;
  color: #d4d4d4;
  background: #252526;
  flex-shrink: 0;
}
.diff-head .t-icon { cursor: pointer; color: #9e9e9e; }
.diff-head .t-icon:hover { color: #fff; }
.diff-body {
  flex: 1;
  margin: 0;
  padding: 8px 12px;
  font-family: Menlo, Monaco, monospace;
  font-size: 12px;
  line-height: 1.5;
  color: #d4d4d4;
  overflow: auto;
  white-space: pre-wrap;
  word-break: break-all;
}
</style>
