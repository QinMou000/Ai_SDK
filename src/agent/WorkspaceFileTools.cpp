#include "agent/WorkspaceFileTools.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace aiSDK {
namespace {

namespace fs = std::filesystem;

// 本文件集中实现文件工具的防御边界，ReAct 循环本身不处理路径、编码或文件类型细节。
// 调用方在构造 Agent 时给出根目录，该根目录在注册时被规范化为不可变绝对路径。
// 每次模型 Tool Call 提供的路径都视为不可信输入，不能依赖模型遵守工具描述。
// 所有可访问路径都必须先确认是相对路径，再根据操作类型规范化到真实文件系统对象。
// 读取和覆盖已有文件使用 canonical，以便识别符号链接最终指向的位置。
// 新建文件先规范化其已有父目录，再拼接单一文件名，避免不存在目标无法 canonical 的限制。
// 两类解析均在完成后检查最终路径是否仍位于调用方授权根目录之内。
// 根目录归属采用路径分量比较，不根据字符串前缀判断，避免 C:\work 与 C:\workspace 混淆。
// 根目录外的任何路径都以工具失败返回，而不让异常穿透至 Agent 终止整个任务。
// .git 目录被隐藏，防止模型通过文件工具读取仓库对象、配置或工作树元数据。
// .env 与 .env.* 被隐藏，防止常见环境密钥文件成为模型上下文的一部分。
// 常见私钥文件名和扩展名也被拒绝，即使调用方误把项目根目录授权给了 Agent。
// 这些敏感路径规则是额外防线，不能替代调用方选择最小必要工作区这一原则。
// 文件工具不提供删除、移动、复制、权限修改或 Shell 命令，因此不会扩展到系统管理职责。
// 文件工具不支持二进制内容，避免将不可读数据或极大载荷注入后续模型请求。
// UTF-8 校验同时拒绝 NUL，ToolMessage 承载字符串时不会因 C 风格边界产生截断歧义。
// 文件大小上限在读取前检查，也在写入前检查，控制单个 Observation 的内存与提示词规模。
// 目录条目数量上限限制一次列举的返回量，防止大目录导致模型上下文失控。
// 目录列举只访问单层，不递归跟随目录树，因此工具名称与实际 I/O 范围保持一致。
// 目录中的符号链接条目不会返回给模型，避免下一步提示模型尝试间接访问外部位置。
// 已有文件写入只允许普通文件，目录、设备和其他非普通对象均不能作为文本目标。
// 新建工具只接受不存在目标，避免模型把“创建”误用成隐式覆盖操作。
// 覆盖工具只接受已存在目标，避免模型拼错路径时无提示地产生新文件。
// 替换工具要求查找文本恰好出现一次，减少模型在不理解完整文件时的多处误改风险。
// 替换后的内容仍按写入规则检查 UTF-8 与大小，防止修改路径绕过通用文本边界。
// 工具结果只返回相对路径、动作与字节数；读取内容只在显式 read 操作中提供。
// 所有结果经现有 ToolRegistry 收敛为 ToolResult，保持与自定义工具相同的失败协议。
// 本文件不捕获 ToolRegistry 的异常转换职责，内部只抛出明确的标准异常说明失败原因。
// 根目录和容量配置按值复制进每个 handler，注册后外部变量变化不能改变授权范围。
// 重复工具名称在注册前一次性检查，避免出现半套文件工具已注册、半套失败的状态。
// Agent 层负责 Low 风险策略；这里固定声明 Low 仅表达用户已经作出的受限工作区授权决定。
// 文件系统在进程外仍可能并发变化，路径解析和实际 I/O 之间不能承诺事务性隔离。
// 当文件被并发删除、截断或替换时，工具宁可返回失败，也不把不完整内容当作成功 Observation。
// 本实现不创建父目录；目录结构属于调用方或模型经明确工具能力之外的预先准备工作。
// 根目录本身可以用于 list_directory，但不能作为 read、write、create 或 replace 的文件路径。
// 所有文件名比较仅为敏感规则做 ASCII 小写化，不会修改真实路径的大小写或 Unicode 语义。
// 路径显示使用 generic_string，便于模型在不同平台上获得稳定的正斜杠相对路径。
// 目录输出按路径排序，避免底层文件系统迭代顺序让模型输入和测试结果不稳定。
// 模型传入的 JSON 只读取规定字符串字段，缺失或类型不匹配由参数边界立即拒绝。
// 工具 Schema 关闭 additionalProperties，指导支持 Schema 的模型不要发送未实现参数。
// 运行时仍手工检查字段，因为模型输出属于不可信输入，Schema 不能替代本地校验。
// 目录工具允许省略 path 表示根目录，这是唯一允许空路径的操作语义。
// 其他文件工具拒绝空路径，避免把文件操作悄然解释成工作区根目录。
// 路径含 NUL 被拒绝，防止不同运行库在路径末尾截断后看到与校验不同的对象。
// Windows 根名称和根目录分别检查，确保盘符、UNC 路径等绝对形式不会进入拼接逻辑。
// canonical 失败统一表达为路径不可用，不把平台特定错误码或绝对敏感信息回填给模型。
// 文件打开失败会保留受控的相对任务上下文，便于模型在下一轮选择其他文件或解释失败。
// 文本读取采用二进制模式，避免 Windows 自动换行转换改变实际 UTF-8 字节数与上限判断。
// 定长读取以 file_size 为准，因此文件在检查后被截断时会触发读取失败而不是返回部分内容。
// 定长读取也避免依赖不同标准库对流式迭代到 EOF 后状态位的细微差异。
// 文本写入采用二进制截断模式；上层工具语义已经区分创建和覆盖，底层不再猜测意图。
// 写入流关闭后再次检查状态，确保缓冲区刷新失败不会被错误报告成成功。
// 创建与检查现有文件之间仍可能存在外部竞争；首版不提供跨进程锁或原子替换协议。
// 该限制适合小型单进程 Agent MVP，未来可在此组件内部增加原子写入策略而不改 Agent Loop。
// 工具名称、风险等级和参数 Schema 都集中在注册函数，便于未来抽取审批器或 MCP 适配层。
// WorkspaceContext 是内部值对象，不出现在公开头文件，避免泄露路径解析实现细节。
// 每个 helper 只承担一种检查或 I/O 职责，使安全策略可按路径、编码和容量维度独立审查。
// 失败文案使用中文并带操作语境，既能让模型恢复，也能让本地调用方定位错误边界。
// 本实现不会把工作区根目录拼进成功结果，降低模型获得绝对本机路径的机会。
// 读取结果中的内容仍可能是调用方授权数据；是否把它提供给模型由调用任务和根目录选择决定。
// Trace 默认不会记录文件参数和结果，只有调用方配置脱敏器才会把安全裁剪后的详情导出。
// 目录内容可能在列举过程中变化；迭代错误会停止并返回失败，不伪造完整的目录快照。
// 目录条目类型只区分 directory 与 file，其他条目不会被用作后续文本读写目标。
// 普通文件检查拒绝 FIFO、套接字和设备文件，避免文本工具意外阻塞或触发特殊设备行为。
// 文件尺寸以字节而非字符计算，UTF-8 多字节字符也必须计入同一内存与提示词预算。
// 替换工具按字节子串精确匹配，不解释正则表达式，也不根据自然语言猜测近似位置。
// 查找串为空会产生无限匹配位置，因此在替换前被明确拒绝。
// 多处匹配会整体失败，首版不实现“替换第 N 处”以避免额外位置选择协议。
// 新建文件要求父目录已经存在，避免工具暗中构造目录树而扩大可修改的状态范围。
// 注册根目录也不得是敏感目录，避免调用方显式把 .git 或 .env 容器直接授权给 Agent。
// 公共选项可调整容量用于未来受控场景，但零值被拒绝以防得到无意义或绕过意图的配置。
// 这些约束均独立于模型供应商，DeepSeek、测试 Provider 和后续 MCP 适配器共享同一安全实现。
// 公开头文件只暴露注册选项与函数，所有实际路径、流和编码处理细节局限在此编译单元。
// 这使调用方无法借助内部 helper 跳过根目录或敏感路径检查，只能经注册工具进入统一边界。
// 注册过程本身不进行目录扫描，避免构造 Agent 时意外读取工作区文件内容或产生额外启动成本。
// 工具描述明确了单层列举与文本限制，模型提示与实际 handler 的约束保持同一事实来源。
// 失败结果不包含文件内容；读取成功才会返回调用方已授权且经过 UTF-8 校验的文本。
// 覆盖写入不会自动创建备份，调用方需要更强回滚能力时应在工作区策略层显式实现版本控制。
// 本模块不缓存读取内容，连续模型调用若需再次读取将重新访问文件系统并获得当前受限状态。
// 每个 handler 都是同步函数，与 ToolExecutor 的串行执行语义一致，不引入后台线程或异步生命周期。
// 文件权限变化、锁定或目录删除会转换为当前 ToolResult 失败，Agent 可让模型选择其他恢复路径。
// 未来若支持审批，应在调用 handler 前拦截而不是在 I/O 后回滚，保持此组件纯粹执行已授权动作。
// 未来若支持记忆，写入工具结果仍应以相同受限结构记录，不能绕过本模块保存原始绝对路径。
// 该模块的测试覆盖正常路径、容量、编码、敏感名与根逃逸，平台特权限制另在任务记录中说明。

struct WorkspaceContext {
    // root 已是 canonical 目录，后续 helper 不接受外部未规范化根目录。
    fs::path root;
    // 两个配额均按字节或条目计数，避免把模型 token 估算引入文件系统边界。
    std::size_t max_file_bytes = 0U;
    std::size_t max_directory_entries = 0U;
};

// lowerAscii 只接收从 path::string 得到的字节序列，敏感名称本身均为 ASCII。
// 非 ASCII 文件名不会被错误转换，也不会因区域设置产生不同的敏感规则判断。
// 返回新字符串而非修改 path，保证后续 canonical、显示和实际 I/O 仍使用原始路径对象。
// lowerAscii 只用于文件名策略匹配，路径本身仍保留操作系统原有大小写。
std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool hasSuffix(const std::string& value, const std::string& suffix) {
    // 私钥扩展名匹配使用完整小写文件名，避免把目录名或中间片段误视为敏感文件。
    // 空后缀在当前调用点不存在，函数保留一般语义以便将来增加固定扩展规则。
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// isWithinRoot 在规范化后的路径上判断归属，不能依赖原始字符串是否含有 ../。
// canonical 根和候选路径应来自同一文件系统命名空间，调用方在进入此函数前负责取得它们。
// relative 为空时单独比较根路径，兼容不同标准库对相同路径返回空串或点分量的实现。
// 根名称、根目录和 .. 任一存在都说明候选无法作为授权根的后代，必须拒绝。
// 此函数不检查敏感名称；位置归属与文件名策略保持两个可独立审计的职责。
bool isWithinRoot(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    if(relative.empty()) {
        return candidate == root;
    }
    if(relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
        return false;
    }
    for(const fs::path& component : relative) {
        if(component == "..") {
            return false;
        }
    }
    return true;
}

// isSensitivePath 拒绝仓库元数据、环境密钥和常见私钥文件。
// 这只是工作区工具的额外保险，调用方仍应选择包含最少敏感数据的根目录。
// 相对分量逐个检查，因此 .git/objects 或 .env.local/child 等嵌套路径同样会被挡住。
// 文件名扩展名在路径归属已确认后再检查，避免根外候选影响敏感规则的错误信息。
// 本规则不试图识别所有秘密文件类型；它以低误报的常见模式覆盖最危险的默认情况。
// 新增业务秘密模式时应只扩充本函数并在文件测试中增加对应的拒绝用例。
bool isSensitivePath(const fs::path& root, const fs::path& candidate) {
    const fs::path relative = candidate.lexically_relative(root);
    for(const fs::path& component : relative) {
        const std::string name = lowerAscii(component.string());
        if(name == ".git" || name == ".env" || name.rfind(".env.", 0U) == 0U) {
            return true;
        }
    }

    const std::string filename = lowerAscii(candidate.filename().string());
    return filename == "id_rsa" || filename == "id_ed25519" || hasSuffix(filename, ".pem") ||
           hasSuffix(filename, ".key") || hasSuffix(filename, ".pfx") || hasSuffix(filename, ".p12");
}

std::string requiredString(const nlohmann::json& arguments, const char* key) {
    // requiredString 是 JSON 到文件系统边界的第一道类型检查，不接受 null、数字或对象自动转换。
    // 异常会由 ToolRegistry 转为失败 ToolResult，模型可在下一轮修正字段而无需结束 Agent。
    if(!arguments.contains(key) || !arguments.at(key).is_string()) {
        throw std::invalid_argument(std::string("工具参数缺少字符串字段: ") + key);
    }
    return arguments.at(key).get<std::string>();
}

std::string optionalString(const nlohmann::json& arguments, const char* key) {
    // optionalString 只服务 list_directory 的可选 path，缺失字段被解释为根目录而非错误。
    // 若字段存在却类型错误仍拒绝，避免模型把对象或数组序列化成意外路径文本。
    if(!arguments.contains(key)) {
        return "";
    }
    if(!arguments.at(key).is_string()) {
        throw std::invalid_argument(std::string("工具参数必须是字符串: ") + key);
    }
    return arguments.at(key).get<std::string>();
}

fs::path relativePathFromString(const std::string& value, bool allow_empty) {
    // 路径字符串校验发生在 filesystem::path 构造后之前，先隔离不同平台的根路径解析差异。
    // allow_empty 不是“任意空路径可用”的开关，只由目录列举在其公开语义下传入 true。
    // 返回的相对 path 尚未验证存在性；读取、写入和新建各自继续完成适合自身的解析流程。
    if(value.find('\0') != std::string::npos) {
        throw std::invalid_argument("文件路径不能包含 NUL 字符");
    }
    if(value.empty()) {
        if(allow_empty) {
            return fs::path{"."};
        }
        throw std::invalid_argument("文件路径不能为空");
    }

    const fs::path path(value);
    if(path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
        throw std::invalid_argument("文件路径必须是工作区内的相对路径");
    }
    return path;
}

fs::path canonicalRoot(const fs::path& root) {
    // 根目录必须在注册时存在，避免 handler 在第一次模型调用时才暴露配置错误。
    // canonical 会解析根目录自身的链接，使调用方不能通过符号链接把授权范围伪装成另一个目录。
    // 敏感根检查使用根父目录作为比较基准，确保名为 .git 的根本身也被识别。
    if(root.empty()) {
        throw std::invalid_argument("工作区根目录不能为空");
    }

    std::error_code error;
    const fs::path normalized = fs::canonical(root, error);
    if(error || !fs::is_directory(normalized, error)) {
        throw std::invalid_argument("工作区根目录不存在或不是目录: " + root.string());
    }
    if(isSensitivePath(normalized.parent_path(), normalized)) {
        throw std::invalid_argument("工作区根目录不能是敏感目录");
    }
    return normalized;
}

void requireSafePath(const WorkspaceContext& context, const fs::path& candidate) {
    // 位置检查先执行，避免根外路径进入敏感名称判断后产生误导性的“敏感”结论。
    // 成功返回仅表示路径可用于下一步类型或 I/O 检查，不代表文件一定存在或可读写。
    if(!isWithinRoot(context.root, candidate)) {
        throw std::runtime_error("文件路径越过工作区根目录");
    }
    if(isSensitivePath(context.root, candidate)) {
        throw std::runtime_error("文件路径属于受保护的敏感位置");
    }
}

fs::path resolveExistingPath(const WorkspaceContext& context, const std::string& raw_path, bool expect_directory) {
    // 已有对象必须 canonical，只有这样符号链接、点分量和大小写别名才会统一到真实位置。
    // directory 与 regular file 的类型要求由调用工具传入，防止 list/read/write 共享时放宽对象类型。
    // canonical 失败既可能是不存在也可能是权限或链接异常；模型获得统一失败即可选择其他路径。
    const fs::path relative = relativePathFromString(raw_path, expect_directory);
    std::error_code error;
    const fs::path normalized = fs::canonical(context.root / relative, error);
    if(error) {
        throw std::runtime_error("文件路径不存在或无法规范化: " + raw_path);
    }
    requireSafePath(context, normalized);

    const bool expected_type =
        expect_directory ? fs::is_directory(normalized, error) : fs::is_regular_file(normalized, error);
    if(error || !expected_type) {
        throw std::runtime_error(expect_directory ? "目标不是目录: " + raw_path : "目标不是普通文件: " + raw_path);
    }
    return normalized;
}

fs::path resolveNewPath(const WorkspaceContext& context, const std::string& raw_path) {
    // 新目标不能直接 canonical，因为它按 create 语义尚不存在；只规范化已存在父目录。
    // requested 保留词法归一化结果以识别 ../，normalized_parent 则解决父目录链接后的真实归属。
    // 最终文件名从 requested 取得，保证父目录 canonical 后不会把原始路径中其余分量重新引入。
    // exists 检查将创建与覆盖严格分开；外部并发创建会让实际 open 失败而不会被误报告为覆盖成功。
    const fs::path relative = relativePathFromString(raw_path, false);
    const fs::path requested = (context.root / relative).lexically_normal();
    const fs::path parent = requested.parent_path();

    std::error_code error;
    const fs::path normalized_parent = fs::canonical(parent, error);
    if(error || !fs::is_directory(normalized_parent, error)) {
        throw std::runtime_error("目标父目录不存在或不是目录: " + raw_path);
    }
    requireSafePath(context, normalized_parent);

    const fs::path normalized = normalized_parent / requested.filename();
    if(normalized.filename().empty() || normalized.filename() == "." || normalized.filename() == "..") {
        throw std::invalid_argument("新建文件必须包含有效文件名");
    }
    requireSafePath(context, normalized);
    if(fs::exists(normalized, error)) {
        throw std::runtime_error("文件已存在，不能重复创建: " + raw_path);
    }
    if(error) {
        throw std::runtime_error("无法检查目标文件状态: " + raw_path);
    }
    return normalized;
}

// isValidUtf8 拒绝二进制 NUL、截断序列、过长编码和 UTF-16 代理区。
// 文件工具只接受可安全直接回填给模型的 UTF-8 文本，避免二进制内容污染对话消息。
// ASCII 快路径让常见源码、配置和说明文件无需额外解码分配。
// 多字节首字节范围排除 C0/C1 与 F5 以上值，防止非法长度和 Unicode 范围外编码。
// 第二字节特殊范围排除过长编码、代理区与超过 U+10FFFF 的编码，其他续字节统一验证高位。
// 校验按字节前进，不把 UTF-8 文本转换为宽字符，避免平台代码页或本地化设置参与安全判断。
bool isValidUtf8(const std::string& value) {
    for(std::size_t index = 0; index < value.size();) {
        const unsigned char first = static_cast<unsigned char>(value[index]);
        if(first == 0U) {
            return false;
        }
        if(first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        if(first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1U;
        } else if(first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2U;
        } else if(first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3U;
        } else {
            return false;
        }

        if(index + continuation_count >= value.size()) {
            return false;
        }
        const unsigned char second = static_cast<unsigned char>(value[index + 1U]);
        if((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second > 0x9FU) ||
           (first == 0xF0U && second < 0x90U) || (first == 0xF4U && second > 0x8FU)) {
            return false;
        }
        for(std::size_t offset = 1U; offset <= continuation_count; ++offset) {
            if((static_cast<unsigned char>(value[index + offset]) & 0xC0U) != 0x80U) {
                return false;
            }
        }
        index += continuation_count + 1U;
    }
    return true;
}

std::string readUtf8TextFile(const WorkspaceContext& context, const fs::path& path) {
    // file_size 在打开前用于容量预算；随后的定长读取仍检查实际字节数以防并发截断。
    // streamsize 上限额外检查使可配置配额不会在 size_t 到流接口转换时发生截断。
    // 空文件是合法 UTF-8 文本，会返回空 content，由模型根据工具结果自行解释。
    // 读取失败不返回部分内容，避免模型基于被并发修改的半个文件继续作出写入决策。
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if(error) {
        throw std::runtime_error("无法读取文件大小: " + path.string());
    }
    if(size > context.max_file_bytes) {
        throw std::runtime_error("文件大小超过工作区工具限制");
    }
    if(size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("文件大小超出可读取范围");
    }

    std::ifstream input(path, std::ios::binary);
    if(!input) {
        throw std::runtime_error("无法打开文件读取: " + path.string());
    }
    // 按已校验的大小定长读取，既避免迭代器在 MSVC 上不设置 eofbit 的误判，
    // 也能在文件被并发截断时通过实际读取字节数明确报错而不回传不完整文本。
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if(input.bad() || input.gcount() != static_cast<std::streamsize>(content.size())) {
        throw std::runtime_error("读取文件失败: " + path.string());
    }
    if(!isValidUtf8(content)) {
        throw std::runtime_error("文件不是受支持的 UTF-8 文本");
    }
    return content;
}

void writeUtf8TextFile(const WorkspaceContext& context, const fs::path& path, const std::string& content) {
    // 容量和编码在打开文件前验证，避免无效模型输入先截断目标文件再报告错误。
    // 调用方已经根据 create/write/replace 完成存在性检查，底层 helper 只负责受控文本写入。
    // binary 模式确保写入字节与 content.size 一致，不受 Windows 文本模式换行转换影响。
    // 关闭流后再检查状态，使磁盘满、权限变化等延迟错误不会形成错误的成功 Observation。
    if(content.size() > context.max_file_bytes) {
        throw std::invalid_argument("文本内容超过工作区工具限制");
    }
    if(!isValidUtf8(content)) {
        throw std::invalid_argument("文本内容必须是无 NUL 的 UTF-8 字符串");
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if(!output) {
        throw std::runtime_error("无法打开文件写入: " + path.string());
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if(!output) {
        throw std::runtime_error("写入文件失败: " + path.string());
    }
}

std::string relativeDisplayPath(const WorkspaceContext& context, const fs::path& path) {
    // 对模型暴露相对路径而非绝对根目录，减少本机目录结构进入后续提示词和日志的范围。
    return path.lexically_relative(context.root).generic_string();
}

Tool makeTool(std::string name, std::string description, nlohmann::json parameters) {
    // 文件工具风险等级在单一工厂处固定，新增工具时不应遗漏与 Agent 策略相配套的元数据。
    // handler 不保存在 Tool 对象中，真实执行函数仍由 ToolRegistry 按相同名称单独维护。
    return Tool{std::move(name), std::move(description), std::move(parameters), ToolRiskLevel::Low};
}

}  // namespace

void registerWorkspaceFileTools(ToolRegistry& registry, const WorkspaceFileToolOptions& options) {
    // 零配额通常是调用方配置遗漏；注册阶段拒绝比让每次模型调用都失败更容易诊断。
    // canonicalRoot 在任何 Tool 对象注册前执行，根目录无效时注册表保持完全不变。
    if(options.max_file_bytes == 0U || options.max_directory_entries == 0U) {
        throw std::invalid_argument("工作区文件工具限制必须大于零");
    }

    WorkspaceContext context{canonicalRoot(options.root), options.max_file_bytes, options.max_directory_entries};
    const std::vector<std::string> names{
        "list_directory", "read_text_file", "create_text_file", "write_text_file", "replace_text_in_file",
    };
    for(const std::string& name : names) {
        // ToolRegistry 对同名注册支持替换，但文件工具需要成套一致语义，因此选择显式拒绝占用。
        if(registry.hasTool(name)) {
            throw std::invalid_argument("工作区工具名称已被占用: " + name);
        }
    }

    // 所有 handler 以值捕获不可变工作区上下文；注册完成后外部不能修改其根目录或大小限制。
    registry.registerTool(
        makeTool("list_directory", "列出工作区内指定目录的单层文件和目录，不会递归访问或显示受保护条目。",
                 nlohmann::json{
                     {"type",                 "object"                                                                                 },
                     {"properties",
                      {{"path", {{"type", "string"}, {"description", "工作区内的相对目录路径，省略时表示根目录"}}}}},
                     {"additionalProperties", false                                                                                    }
    }),
        [context](const nlohmann::json& arguments) {
            // 仅 path 是可选字段；缺失时 resolveExistingPath 将受控地解析为授权根目录。
            const fs::path directory = resolveExistingPath(context, optionalString(arguments, "path"), true);
            std::vector<nlohmann::json> entries;
            std::error_code error;
            for(const fs::directory_entry& entry : fs::directory_iterator(directory, error)) {
                // 迭代器错误可能在构造后才出现，循环内必须再次检查而不能假定目录快照稳定。
                if(error) {
                    throw std::runtime_error("遍历目录失败: " + directory.string());
                }
                if(entry.is_symlink(error)) {
                    // 不显示链接条目使模型不会把它们当作可继续读取的工作区文件。
                    error.clear();
                    continue;
                }
                if(error) {
                    throw std::runtime_error("读取目录条目失败: " + directory.string());
                }
                // Windows 目录联接点不一定被 is_symlink 标识；必须再规范化实际目标，
                // 否则列举结果会把工作区外目录伪装成后续可读取的本地条目。
                const fs::path normalized_entry = fs::canonical(entry.path(), error);
                if(error) {
                    // 无法解析的重解析点或并发删除条目不进入模型上下文，避免泄露不稳定路径。
                    error.clear();
                    continue;
                }
                if(!isWithinRoot(context.root, normalized_entry) || isSensitivePath(context.root, normalized_entry)) {
                    // 最终目标越界或敏感时静默隐藏；列表不会把外部位置作为可探索目录提示给模型。
                    continue;
                }
                if(isSensitivePath(context.root, entry.path())) {
                    // 敏感条目连名称也不返回，避免目录结果成为环境配置存在性的侧信道。
                    continue;
                }
                if(entries.size() >= context.max_directory_entries) {
                    // 超限整体失败而不是静默截断，模型可明确选择更小的已知子目录。
                    throw std::runtime_error("目录条目数量超过工作区工具限制");
                }

                const bool is_directory = fs::is_directory(normalized_entry, error);
                if(error) {
                    throw std::runtime_error("读取目录条目类型失败: " + entry.path().string());
                }
                entries.push_back({
                    {"path", relativeDisplayPath(context, entry.path())},
                    {"type", is_directory ? "directory" : "file"},
                });
            }
            std::sort(entries.begin(), entries.end(), [](const nlohmann::json& left, const nlohmann::json& right) {
                // 排序键仅使用已脱离绝对根的 display path，不比较文件系统本地化名称元数据。
                return left.at("path").get<std::string>() < right.at("path").get<std::string>();
            });
            return ToolResult::successResult({
                {"path", relativeDisplayPath(context, directory)},
                {"entries", entries},
            });
        });

    registry.registerTool(
        makeTool("read_text_file", "读取工作区内一个受配置大小限制的 UTF-8 文本文件。",
                 nlohmann::json{
                     {"type",                 "object"                                                                              },
                     {"properties",           {{"path", {{"type", "string"}, {"description", "工作区内的相对文件路径"}}}}},
                     {"required",             {"path"}                                                                              },
                     {"additionalProperties", false                                                                                 }
    }),
        [context](const nlohmann::json& arguments) {
            // read 只接受普通文件；目录、链接和不存在对象均在 resolveExistingPath 处失败。
            const fs::path path = resolveExistingPath(context, requiredString(arguments, "path"), false);
            const std::string content = readUtf8TextFile(context, path);
            return ToolResult::successResult({
                {"path", relativeDisplayPath(context, path)},
                {"content", content},
            });
        });

    registry.registerTool(
        makeTool("create_text_file", "在工作区内新建一个不存在的 UTF-8 文本文件，不会覆盖已有文件。",
                 nlohmann::json{
                     {"type",                 "object"                                                           },
                     {"properties",           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
                     {"required",             {"path", "content"}                                                },
                     {"additionalProperties", false                                                              }
    }),
        [context](const nlohmann::json& arguments) {
            // create 在写入前检查目标不存在，保证同名文件不会被本工具隐式截断。
            const fs::path path = resolveNewPath(context, requiredString(arguments, "path"));
            const std::string content = requiredString(arguments, "content");
            writeUtf8TextFile(context, path, content);
            return ToolResult::successResult({
                {"path", relativeDisplayPath(context, path)},
                {"bytes", content.size()},
                {"action", "created"},
            });
        });

    registry.registerTool(
        makeTool("write_text_file", "覆盖工作区内一个已经存在的 UTF-8 文本文件，不会隐式新建文件。",
                 nlohmann::json{
                     {"type",                 "object"                                                           },
                     {"properties",           {{"path", {{"type", "string"}}}, {"content", {{"type", "string"}}}}},
                     {"required",             {"path", "content"}                                                },
                     {"additionalProperties", false                                                              }
    }),
        [context](const nlohmann::json& arguments) {
            // write 复用已有路径解析，明确要求调用方先知道目标已经存在。
            const fs::path path = resolveExistingPath(context, requiredString(arguments, "path"), false);
            const std::string content = requiredString(arguments, "content");
            writeUtf8TextFile(context, path, content);
            return ToolResult::successResult({
                {"path", relativeDisplayPath(context, path)},
                {"bytes", content.size()},
                {"action", "written"},
            });
        });

    registry.registerTool(
        makeTool(
            "replace_text_in_file", "在工作区内 UTF-8 文本文件中精确替换唯一的一处文本；零处或多处匹配都会失败。",
            nlohmann::json{
                {"type",                 "object"                                                                     },
                {"properties",
                 {{"path", {{"type", "string"}}}, {"search", {{"type", "string"}}}, {"replace", {{"type", "string"}}}}},
                {"required",             {"path", "search", "replace"}                                                },
                {"additionalProperties", false                                                                        }
    }),
        [context](const nlohmann::json& arguments) {
            // replace 先完整读取再校验匹配数，任何不确定匹配都不会触发文件写入。
            const fs::path path = resolveExistingPath(context, requiredString(arguments, "path"), false);
            const std::string search = requiredString(arguments, "search");
            const std::string replacement = requiredString(arguments, "replace");
            if(search.empty()) {
                throw std::invalid_argument("替换文本不能为空");
            }

            std::string content = readUtf8TextFile(context, path);
            const std::size_t first_match = content.find(search);
            // npos 是零匹配；该情况不等同于创建或追加，必须保留为可恢复失败。
            if(first_match == std::string::npos) {
                throw std::runtime_error("文件中未找到待替换文本");
            }
            if(content.find(search, first_match + search.size()) != std::string::npos) {
                // 多处命中代表模型没有指定唯一位置；首版拒绝而不是选择第一处避免误改。
                throw std::runtime_error("待替换文本匹配多处，拒绝修改文件");
            }
            content.replace(first_match, search.size(), replacement);
            // 内容替换后仍通过统一写入 helper，编码和大小预算不会因 replace 路径被绕过。
            writeUtf8TextFile(context, path, content);
            return ToolResult::successResult({
                {"path", relativeDisplayPath(context, path)},
                {"bytes", content.size()},
                {"action", "replaced"},
            });
        });
}

}  // namespace aiSDK
