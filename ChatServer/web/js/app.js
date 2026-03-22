// API基础URL
const API_BASE = '';

// 状态管理
const state = {
    sessions: [],
    currentSessionId: null,
    models: [],
    selectedModel: null,
    isStreaming: false
};

// DOM元素
const elements = {
    sessionList: document.getElementById('sessionList'),
    welcomePage: document.getElementById('welcomePage'),
    chatPage: document.getElementById('chatPage'),
    chatMessages: document.getElementById('chatMessages'),
    messageInput: document.getElementById('messageInput'),
    charCount: document.getElementById('charCount'),
    sendBtn: document.getElementById('sendBtn'),
    newChatBtn: document.getElementById('newChatBtn'),
    welcomeNewChatBtn: document.getElementById('welcomeNewChatBtn'),
    modelModal: document.getElementById('modelModal'),
    modelGrid: document.getElementById('modelGrid'),
    closeModal: document.getElementById('closeModal'),
    cancelBtn: document.getElementById('cancelBtn'),
    confirmBtn: document.getElementById('confirmBtn')
};

// 格式化时间
function formatTime(timestamp) {
    const date = new Date(timestamp * 1000);
    const now = new Date();
    const isToday = date.toDateString() === now.toDateString();

    if (isToday) {
        return date.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
    } else {
        return date.toLocaleDateString('zh-CN', { month: '2-digit', day: '2-digit' }) + ' ' +
               date.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
    }
}

// 格式化时间戳（毫秒）
function formatTimestamp(timestamp) {
    const date = new Date(timestamp);
    return date.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
}

// API请求
async function apiRequest(url, options = {}) {
    try {
        const response = await fetch(API_BASE + url, {
            ...options,
            headers: {
                'Content-Type': 'application/json',
                ...options.headers
            }
        });

        if (!response.ok) {
            throw new Error(`HTTP ${response.status}`);
        }

        return await response.json();
    } catch (error) {
        console.error('API请求失败:', error);
        throw error;
    }
}

// 加载会话列表
async function loadSessions() {
    try {
        const result = await apiRequest('/api/sessions');
        if (result.success) {
            state.sessions = result.data || [];
            renderSessionList();
        }
    } catch (error) {
        console.error('加载会话列表失败:', error);
    }
}

// 加载可用模型
async function loadModels() {
    try {
        const result = await apiRequest('/api/models');
        if (result.success) {
            state.models = result.data || [];
            renderModelList();
        }
    } catch (error) {
        console.error('加载模型列表失败:', error);
    }
}

// 渲染会话列表
function renderSessionList() {
    if (state.sessions.length === 0) {
        elements.sessionList.innerHTML = '<div class="empty-state">暂无会话</div>';
        return;
    }

    elements.sessionList.innerHTML = state.sessions.map(session => `
        <div class="session-item ${session.id === state.currentSessionId ? 'active' : ''}" data-id="${session.id}">
            <div class="session-info">
                <div class="session-title">${escapeHtml(session.first_user_message || '新对话')}</div>
                <div class="session-meta">
                    <span class="session-time">${formatTime(session.updated_at)}</span>
                    <span class="session-model">${escapeHtml(session.model)}</span>
                </div>
            </div>
            <button class="session-delete" data-id="${session.id}" title="删除会话">&times;</button>
        </div>
    `).join('');

    // 绑定事件
    document.querySelectorAll('.session-item').forEach(item => {
        item.addEventListener('click', (e) => {
            if (!e.target.classList.contains('session-delete')) {
                selectSession(item.dataset.id);
            }
        });
    });

    document.querySelectorAll('.session-delete').forEach(btn => {
        btn.addEventListener('click', (e) => {
            e.stopPropagation();
            deleteSession(btn.dataset.id);
        });
    });
}

// 渲染模型列表
function renderModelList() {
    elements.modelGrid.innerHTML = state.models.map((model, index) => `
        <label class="model-card ${index === 0 ? 'selected' : ''}">
            <input type="radio" name="model" value="${model.name}" ${index === 0 ? 'checked' : ''}>
            <div class="model-name">${escapeHtml(model.name)}</div>
            <div class="model-desc">${escapeHtml(model.desc)}</div>
        </label>
    `).join('');

    // 绑定选择事件
    document.querySelectorAll('.model-card').forEach(card => {
        card.addEventListener('click', () => {
            document.querySelectorAll('.model-card').forEach(c => c.classList.remove('selected'));
            card.classList.add('selected');
            card.querySelector('input').checked = true;
        });
    });
}

// 选择会话
async function selectSession(sessionId) {
    state.currentSessionId = sessionId;
    const session = state.sessions.find(s => s.id === sessionId);

    if (!session) return;

    // 隐藏欢迎页面，显示聊天页面
    elements.welcomePage.classList.add('hidden');
    elements.chatPage.classList.remove('hidden');

    // 加载历史消息
    await loadHistory(sessionId);

    // 更新选中状态
    renderSessionList();
}

// 加载历史消息
async function loadHistory(sessionId) {
    try {
        const result = await apiRequest(`/api/session/${sessionId}/history`);
        if (result.success) {
            renderMessages(result.data || []);
        }
    } catch (error) {
        console.error('加载历史消息失败:', error);
    }
}

// 渲染消息
function renderMessages(messages) {
    elements.chatMessages.innerHTML = messages.map(msg => createMessageHTML(msg)).join('');

    // 添加复制按钮事件
    addCopyButtonEvents();

    // 滚动到底部
    scrollToBottom();
}

// 创建消息HTML
function createMessageHTML(msg) {
    const isUser = msg.role === 'user';
    const avatarSvg = isUser ?
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M20 21v-2a4 4 0 0 0-4-4H8a4 4 0 0 0-4 4v2"></path><circle cx="12" cy="7" r="4"></circle></svg>' :
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2L2 7L12 12L22 7L12 2Z"></path><path d="M2 17L12 22L22 17"></path><path d="M2 12L12 17L22 12"></path></svg>';

    let content;
    if (isUser) {
        content = escapeHtml(msg.content);
    } else {
        // 使用marked解析Markdown
        content = marked.parse(msg.content, { breaks: true });
    }

    return `
        <div class="message ${msg.role}">
            <div class="message-avatar">${avatarSvg}</div>
            <div class="message-content">
                <div class="message-bubble">${content}</div>
                <div class="message-time">${formatTimestamp(msg.timestamp * 1000)}</div>
            </div>
        </div>
    `;
}

// 添加复制按钮
function addCopyButtonEvents() {
    document.querySelectorAll('.message-bubble pre').forEach(pre => {
        const btn = document.createElement('button');
        btn.className = 'code-copy-btn';
        btn.textContent = '复制';
        btn.addEventListener('click', () => {
            const code = pre.querySelector('code')?.textContent || pre.textContent;
            navigator.clipboard.writeText(code).then(() => {
                btn.textContent = '已复制';
                setTimeout(() => btn.textContent = '复制', 2000);
            });
        });
        pre.appendChild(btn);
    });
}

// 创建新会话
async function createSession() {
    const selectedRadio = document.querySelector('input[name="model"]:checked');
    if (!selectedRadio) {
        alert('请选择一个模型');
        return;
    }

    const modelName = selectedRadio.value;

    try {
        const result = await apiRequest('/api/session', {
            method: 'POST',
            body: JSON.stringify({ model: modelName })
        });

        if (result.success) {
            closeModal();
            await loadSessions();
            selectSession(result.data.session_id);
        } else {
            alert(result.message || '创建会话失败');
        }
    } catch (error) {
        console.error('创建会话失败:', error);
        alert('创建会话失败，请重试');
    }
}

// 删除会话
async function deleteSession(sessionId) {
    if (!confirm('确定要删除这个会话吗？')) {
        return;
    }

    try {
        const result = await apiRequest(`/api/session/${sessionId}`, {
            method: 'DELETE'
        });

        if (result.success) {
            if (state.currentSessionId === sessionId) {
                state.currentSessionId = null;
                elements.chatPage.classList.add('hidden');
                elements.welcomePage.classList.remove('hidden');
            }
            await loadSessions();
        } else {
            alert(result.message || '删除会话失败');
        }
    } catch (error) {
        console.error('删除会话失败:', error);
        alert('删除会话失败，请重试');
    }
}

// 发送消息
async function sendMessage() {
    if (state.isStreaming) return;

    const content = elements.messageInput.value.trim();
    if (!content || !state.currentSessionId) return;

    // 清空输入框
    elements.messageInput.value = '';
    elements.charCount.textContent = '0';

    state.isStreaming = true;
    elements.sendBtn.disabled = true;

    // 添加用户消息到界面
    const userMsg = {
        role: 'user',
        content: content,
        timestamp: Date.now() / 1000
    };
    appendMessage(userMsg);

    // 添加加载中的AI消息
    const loadingMsg = createLoadingMessage();
    elements.chatMessages.appendChild(loadingMsg);
    scrollToBottom();

    try {
        const response = await fetch(API_BASE + '/api/message/async', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                session_id: state.currentSessionId,
                message: content
            })
        });

        if (!response.ok) {
            throw new Error('发送消息失败');
        }

        const reader = response.body.getReader();
        const decoder = new TextDecoder();
        let assistantContent = '';
        let buffer = '';

        while (true) {
            const { done, value } = await reader.read();
            if (done) break;

            buffer += decoder.decode(value, { stream: true });

            // 处理SSE数据
            const lines = buffer.split('\n');
            buffer = lines.pop() || '';

            for (const line of lines) {
                if (line.startsWith('data: ')) {
                    const data = line.slice(6).trim();

                    if (data === '[DONE]') {
                        // 完成
                        loadingMsg.remove();
                        const assistantMsg = {
                            role: 'assistant',
                            content: assistantContent,
                            timestamp: Date.now() / 1000
                        };
                        appendMessage(assistantMsg);
                        await loadSessions(); // 刷新会话列表（更新时间）
                    } else if (data.startsWith('{') && data.endsWith('}')) {
                        // 错误信息
                        try {
                            const error = JSON.parse(data);
                            console.error('Error:', error.error);
                        } catch (e) {}
                    } else {
                        // 内容片段
                        assistantContent += data;
                        updateLoadingMessage(loadingMsg, assistantContent);
                    }
                }
            }
        }
    } catch (error) {
        console.error('发送消息失败:', error);
        loadingMsg.remove();
        alert('发送消息失败，请重试');
    } finally {
        state.isStreaming = false;
        elements.sendBtn.disabled = false;
        elements.messageInput.focus();
    }
}

// 创建加载中的消息
function createLoadingMessage() {
    const div = document.createElement('div');
    div.className = 'message assistant loading';
    div.innerHTML = `
        <div class="message-avatar">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M12 2L2 7L12 12L22 7L12 2Z"></path>
                <path d="M2 17L12 22L22 17"></path>
                <path d="M2 12L12 17L22 12"></path>
            </svg>
        </div>
        <div class="message-content">
            <div class="message-bubble">
                <div class="loading">
                    <div class="spinner"></div>
                    <span>正在思考...</span>
                </div>
            </div>
        </div>
    `;
    return div;
}

// 更新加载中的消息
function updateLoadingMessage(loadingEl, content) {
    const bubble = loadingEl.querySelector('.message-bubble');
    bubble.innerHTML = marked.parse(content, { breaks: true });

    // 添加复制按钮
    bubble.querySelectorAll('pre').forEach(pre => {
        if (!pre.querySelector('.code-copy-btn')) {
            addCopyButtonEvents();
        }
    });

    scrollToBottom();
}

// 添加消息到界面
function appendMessage(msg) {
    const div = document.createElement('div');
    div.innerHTML = createMessageHTML(msg);
    elements.chatMessages.appendChild(div.firstElementChild);
    addCopyButtonEvents();
    scrollToBottom();
}

// 滚动到底部
function scrollToBottom() {
    elements.chatMessages.scrollTop = elements.chatMessages.scrollHeight;
}

// 转义HTML
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

// 打开模型选择弹窗
function openModal() {
    elements.modelModal.classList.remove('hidden');
}

// 关闭模型选择弹窗
function closeModal() {
    elements.modelModal.classList.add('hidden');
}

// 初始化
async function init() {
    // 加载会话列表
    await loadSessions();

    // 加载模型列表
    await loadModels();

    // 绑定事件
    elements.newChatBtn.addEventListener('click', openModal);
    elements.welcomeNewChatBtn.addEventListener('click', openModal);
    elements.closeModal.addEventListener('click', closeModal);
    elements.cancelBtn.addEventListener('click', closeModal);
    elements.confirmBtn.addEventListener('click', createSession);
    elements.modelModal.querySelector('.modal-overlay').addEventListener('click', closeModal);

    // 发送按钮事件
    elements.sendBtn.addEventListener('click', sendMessage);

    // 输入框事件
    elements.messageInput.addEventListener('input', () => {
        elements.charCount.textContent = elements.messageInput.value.length;
    });

    // 键盘事件
    elements.messageInput.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            sendMessage();
        }
    });

    // 自动调整输入框高度
    elements.messageInput.addEventListener('input', () => {
        elements.messageInput.style.height = 'auto';
        elements.messageInput.style.height = Math.min(elements.messageInput.scrollHeight, 200) + 'px';
    });
}

// 启动
document.addEventListener('DOMContentLoaded', init);
