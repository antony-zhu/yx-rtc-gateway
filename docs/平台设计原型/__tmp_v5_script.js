    const roleMeta = {
      platform: {
        avatar: "平",
        userName: "平台管理员",
        userScope: "平台级",
        note: "平台管理员只做平台级管理，不替用户创建项目、设备组或配置。",
        defaultPage: "platformHome",
        menu: [
          { section: "平台", items: [
            ["platformHome", "首", "平台首页"],
            ["users", "用", "用户管理"],
            ["platformDevices", "设", "设备"],
            ["unassigned", "池", "未分配设备"],
            ["productModels", "型", "产品型号"],
            ["firmwares", "固", "固件版本"],
            ["platformAudit", "审", "平台审计"]
          ]}
        ]
      },
      tenant: {
        avatar: "用",
        userName: "用户管理员",
        userScope: "绍兴地铁运营",
        note: "用户管理员只管理本用户下的项目、设备组、设备和配置。",
        defaultPage: "projects",
        menu: [
          { section: "用户端", items: [
            ["projects", "项", "项目管理"],
            ["tenantDevices", "设", "设备配置"],
            ["members", "权", "成员权限"],
            ["tenantTasks", "任", "任务日志"]
          ]}
        ]
      },
      child: {
        avatar: "子",
        userName: "子用户",
        userScope: "仅授权：绍兴地铁 / 1号线站台",
        note: "子用户只看已授权的项目和设备组，按钮会按权限收敛。",
        defaultPage: "childHome",
        menu: [
          { section: "授权范围", items: [
            ["childHome", "首", "我的工作台"],
            ["childProjects", "项", "项目管理"],
            ["childDevices", "设", "设备配置"],
            ["childTasks", "任", "任务日志"]
          ]}
        ]
      }
    };

    const pageMeta = {
      platformHome: ["平台首页", "查看平台级用户、设备接入、产品型号和固件概览"],
      users: ["用户管理", "平台管理员创建用户和用户管理员账号"],
      platformDevices: ["设备", "平台视角显示全部设备，只展示所属用户"],
      unassigned: ["未分配设备", "仅展示未分配设备池，不提供加入项目或设备组"],
      productModels: ["产品型号", "平台管理员维护产品型号，可顺带添加固件版本"],
      firmwares: ["固件版本", "平台管理员维护固件版本，并声明适用产品型号"],
      platformAudit: ["平台审计", "平台级操作和设备接入审计"],
      projects: ["项目管理", "从项目进入设备组，再进入设备和配置"],
      tenantDevices: ["设备配置", "用户范围设备组配置入口，支持配置、编辑、列表、删除、关联和发布"],
      members: ["成员权限", "用户管理员维护子用户和授权范围"],
      tenantTasks: ["任务日志", "本用户设备同步、发布和异常任务"],
      childHome: ["我的工作台", "显示授权范围内可访问的设备"],
      childProjects: ["项目管理", "子用户可访问的项目和设备组"],
      childDevices: ["设备配置", "子用户可查看的设备"],
      childTasks: ["任务日志", "授权范围内任务结果"]
    };

    let currentRole = "platform";
    let currentPage = "platformHome";

    const nav = document.getElementById("nav");
    const content = document.getElementById("content");
    const pageTitle = document.getElementById("pageTitle");
    const pageSub = document.getElementById("pageSub");
    const sideNote = document.getElementById("sideNote");
    const modal = document.getElementById("addDeviceModal");
    const productModal = document.getElementById("productModelModal");

    function tag(text, color) {
      return `<span class="tag ${color}">${text}</span>`;
    }

    function renderShell() {
      const meta = roleMeta[currentRole];
      const activePage = activeMenuPage(currentPage);
      document.getElementById("avatar").textContent = meta.avatar;
      document.getElementById("userName").textContent = meta.userName;
      document.getElementById("userScope").textContent = meta.userScope;
      sideNote.textContent = meta.note;
      nav.innerHTML = meta.menu.map((section) => `
        <div class="nav-section">${section.section}</div>
        ${section.items.map(([id, icon, label]) => `
          <button class="nav-btn ${activePage === id ? "active" : ""}" data-page="${id}" type="button">
            <span class="nav-icon">${icon}</span>${label}
          </button>
        `).join("")}
      `).join("");
      nav.querySelectorAll("[data-page]").forEach((button) => {
        button.addEventListener("click", () => openPage(button.dataset.page));
      });
      document.querySelectorAll(".role-btn").forEach((button) => {
        button.classList.toggle("active", button.dataset.role === currentRole);
      });
    }

    function activeMenuPage(page) {
      if (page === "projectDetail" || page === "groupDetail" || page === "groupConfig") {
        return "projects";
      }
      return page;
    }

    function openPage(id) {
      currentPage = id;
      const meta = pageMeta[id];
      pageTitle.textContent = meta[0];
      pageSub.textContent = meta[1];
      renderShell();
      content.innerHTML = pages[id]();
      bindContentActions();
    }

    function switchRole(role) {
      currentRole = role;
      currentPage = roleMeta[role].defaultPage;
      openPage(currentPage);
    }

    const pages = {
      platformHome: () => `
        <div class="bar">
          <div><h2>平台首页</h2><p>平台管理员关注平台资产和接入状态，不进入用户项目代操作。</p></div>
        </div>
        <div class="grid cols-4" style="margin-bottom:14px;">
          ${metric("用户数量", "18", "启用 16，停用 2", "blue", "平台")}
          ${metric("设备总数", "328", "已绑定 286，未分配 42", "green", "资产")}
          ${metric("产品型号", "6", "启用 5，停用 1", "blue", "型号")}
          ${metric("固件版本", "21", "本月新增 3 个", "amber", "固件")}
        </div>
        <div class="notice warn">
          <strong>权限边界</strong>
          <span>平台管理员可以看到设备属于哪个用户，但不展示用户内部项目和设备组，也不替用户添加设备到组。</span>
        </div>
      `,

      users: () => `
        ${toolbar("用户管理", "平台管理员只能新建用户管理员账号。", ["新建用户管理员"])}
        ${table(["用户名称", "用户管理员", "项目数", "设备数", "状态", "操作"], [
          ["绍兴地铁运营", "tenant_admin_sx", "3", "86", tag("启用", "green"), actions(["详情", "停用"])],
          ["城市水务集团", "tenant_admin_water", "2", "42", tag("启用", "green"), actions(["详情", "停用"])],
          ["越新制造", "tenant_admin_yx", "5", "108", tag("启用", "green"), actions(["详情", "停用"])]
        ])}
      `,

      platformDevices: () => `
        ${toolbar("设备", "平台视角显示全部设备，只展示所属用户，不展示项目和设备组。", ["导出"])}
        ${filters(["搜索设备编号、MAC", "全部用户", "全部产品型号", "全部在线状态"])}
        ${table(["设备编号", "产品型号", "联网方式", "在线状态", "固件版本", "所属用户", "绑定状态", "最后心跳", "操作"], [
          [mono("DTU01B-0092"), "DTU01B", "Ethernet", tag("在线", "green"), "1.2.0", "绍兴地铁运营", tag("已绑定", "blue"), "1 分钟前", actions(["详情", "停用"])],
          [mono("DTU01B-00AA"), "DTU01B", "4G", tag("在线", "green"), "1.1.8", "-", tag("未分配", "amber"), "30 秒前", actions(["详情", "备注"])],
          [mono("DTU01A-0021"), "DTU01A", "WiFi", tag("离线", "red"), "1.0.4", "城市水务集团", tag("已绑定", "blue"), "3 小时前", actions(["详情", "停用"])]
        ])}
      `,

      unassigned: () => `
        ${toolbar("未分配设备池", "只展示平台未分配设备，不提供加入用户项目或设备组。", ["预录入设备", "导出"])}
        <div class="notice warn" style="margin-bottom:14px;">
          <strong>仅展示</strong><span>用户添加设备时在设备组详情输入设备编号，由后端校验是否在这里。</span>
        </div>
        ${table(["设备编号", "产品型号", "来源", "在线状态", "固件版本", "首次入池", "最后心跳", "操作"], [
          [mono("DTU01B-00AA"), "DTU01B", tag("上线注册", "green"), tag("在线", "green"), "1.1.8", "2026-05-13 10:24", "30 秒前", actions(["详情", "备注", "停用"])],
          [mono("DTU01B-00A9"), "DTU01B", tag("预录入", "amber"), tag("离线", "red"), "-", "2026-05-13 09:12", "-", actions(["详情", "备注", "停用"])],
          [mono("DTU01A-00C1"), "DTU01A", tag("用户解绑", "blue"), tag("离线", "red"), "1.0.4", "2026-05-12 16:30", "1 天前", actions(["详情", "备注", "停用"])]
        ])}
      `,

      productModels: () => `
        ${toolbar("产品型号", "产品型号只能由平台管理员添加；点击新增后在弹窗中填写。", ["新增产品型号"], ["openProductModel"])}
        ${table(["产品型号", "产品名称", "硬件类型", "联网能力", "默认固件", "状态", "操作"], [
            ["DTU01B", "4G DTU B 型", "4G 模块迭代版", "4G / Ethernet", "1.2.0", tag("启用", "green"), actions(["编辑", "新增固件"])],
            ["DTU01A", "4G DTU A 型", "4G 模块初版", "4G", "1.0.4", tag("启用", "green"), actions(["编辑", "新增固件"])],
            ["GW-ESP32S3", "ESP32-S3 网关", "通用网关", "WiFi / 4G / Ethernet", "1.1.8", tag("停用", "red"), actions(["详情"])]
        ])}
      `,

      firmwares: () => `
        ${toolbar("固件版本", "固件版本只能由平台管理员添加，且必须声明适用产品型号。", ["新增固件版本"])}
        ${table(["固件版本", "适用产品型号", "文件", "SHA256", "状态", "说明", "操作"], [
          ["1.2.0", "DTU01B", "dtu01b_1.2.0.bin", "a81c...f20e", tag("可用", "green"), "修复 4G 重连", actions(["详情", "停用"])],
          ["1.0.4", "DTU01A", "dtu01a_1.0.4.bin", "b17d...9a42", tag("可用", "green"), "稳定版", actions(["详情", "停用"])],
          ["1.3.0-beta", "DTU01B", "dtu01b_1.3.0.bin", "c21a...77f1", tag("草稿", "amber"), "测试固件", actions(["编辑", "发布可用"])]
        ])}
      `,

      platformAudit: () => `
        ${toolbar("平台审计", "平台级用户、产品型号、固件和设备接入操作记录。", ["导出"])}
        ${table(["时间", "操作人", "对象", "动作", "结果"], [
          ["2026-05-13 15:30", "平台管理员", "产品型号 DTU01B", "新增固件 1.2.0", tag("成功", "green")],
          ["2026-05-13 15:12", "平台管理员", "设备 DTU01B-00A9", "预录入", tag("成功", "green")],
          ["2026-05-13 14:50", "平台管理员", "用户 城市水务集团", "创建用户管理员", tag("成功", "green")]
        ])}
      `,

      projects: () => `
        ${toolbar("项目管理", "项目是用户端一级入口；项目详情不是菜单项。", ["新建项目"])}
        <div class="cards">
          ${projectCard("绍兴地铁样板项目", "4", "32", "6", "openProject")}
          ${projectCard("机房环境监控", "3", "21", "2", "openProject")}
          ${projectCard("车站能耗采集", "2", "33", "5", "openProject")}
        </div>
      `,

      projectDetail: () => `
        ${crumb([["projects", "项目管理"], ["", "绍兴地铁样板项目"]])}
        ${toolbar("绍兴地铁样板项目", "设备组属于项目。先进入设备组，才能编辑设备和配置。", ["新建设备组"])}
        ${table(["设备组名称", "组类型", "产品型号", "固件版本", "组配置版本", "设备数", "未同步", "操作"], [
          ["1 号线站台正式组", tag("正式", "blue"), "DTU01B", "1.2.0", "V100", "28", "5", groupActions("formal", true)],
          ["1 号线站台测试组", tag("测试", "amber"), "DTU01B", "1.2.0", "V108", "4", "1", groupActions("test", true)],
          ["车控室正式组", tag("正式", "blue"), "DTU01B", "1.2.0", "未配置", "0", "0", groupActions("formal", true)]
        ])}
      `,

      groupDetail: () => `
        ${crumb([["projects", "项目管理"], ["projectDetail", "绍兴地铁样板项目"], ["", "1 号线站台测试组"]])}
        ${toolbar("1 号线站台测试组", "组内设备在这里编辑；设备继承当前设备组配置。", ["添加设备", "编辑组配置"], ["openAddDevice", "openGroupConfig"])}
        <div class="split">
          ${table(["设备编号", "联网方式", "在线状态", "设备配置版本", "目标配置版本", "备注", "操作"], [
            [mono("DTU01B-0093"), "4G", tag("在线", "green"), "V108", "V108", "站台东端测试机", actions(["编辑备注", "转移", "移出本组"])],
            [mono("DTU01B-0096"), "Ethernet", tag("离线", "red"), "V105", "V108", "弱网验证", actions(["编辑备注", "创建同步任务", "移出本组"])],
            [mono("DTU01B-0097"), "WiFi", tag("在线", "green"), "V108", "V108", "脚本联调", actions(["编辑备注", "转移", "移出本组"])]
          ])}
          <div class="panel">
            <div class="panel-head"><h3>绑定配置</h3>${tag("参数+脚本一体", "blue")}</div>
            <div class="panel-body grid">
              <div><span class="muted">组配置版本</span><br><strong>V108</strong></div>
              <div><span class="muted">产品型号 / 固件</span><br><strong>DTU01B / 1.2.0</strong></div>
              <div><span class="muted">配置状态</span><br>${tag("草稿", "amber")}</div>
              <div class="notice"><strong>一体版本</strong><span>网络参数、串口、数据流和脚本一起形成 V108。</span></div>
              <button class="btn primary" data-action="openGroupConfig" type="button">进入配置</button>
            </div>
          </div>
        </div>
      `,

      groupConfig: () => `
        ${crumb([["projects", "项目管理"], ["projectDetail", "绍兴地铁样板项目"], ["groupDetail", "1 号线站台测试组"], ["", "设备组配置"]])}
        ${toolbar("设备组配置 V108", "参数和脚本是一体配置版本，绑定当前设备组。", ["保存草稿", "保存并同步测试组", "发布到正式组"])}
        <div class="config-editor">
          <div class="editor-head">
            <div class="tabs">
              <button class="tab active" type="button">基础参数</button>
              <button class="tab" type="button">网络通道</button>
              <button class="tab" type="button">串口/RS485</button>
              <button class="tab" type="button">数据流</button>
              <button class="tab" type="button">脚本</button>
            </div>
            ${tag("草稿", "amber")}
          </div>
          <div class="editor-body">
            <div class="form-grid">
              ${field("产品型号", "DTU01B")}
              ${field("固件版本", "1.2.0")}
              ${field("联网方式", "Ethernet")}
              ${field("MQTT Host", "mqtt.example.com")}
              ${field("RS485 波特率", "115200")}
              ${field("同步策略", "在线立即同步，离线等待上线")}
            </div>
            <div class="field">
              <label>脚本 main.py</label>
              <div class="code">def loop():
    value = rs485.read_holding_registers(slave=1, address=0, count=2)
    net_chl.write({"value": value, "groupConfig": "V108"})</div>
            </div>
          </div>
        </div>
      `,

      tenantDevices: () => `
        ${toolbar("设备配置", "按设备组管理配置、组内设备列表、关联和发布。", ["新建设备组"])}
        ${filters(["搜索设备组、项目", "全部项目", "全部组类型", "全部产品型号"])}
        ${table(["项目", "设备组", "组类型", "产品型号", "固件版本", "组配置版本", "设备数", "操作"], [
          ["绍兴地铁样板项目", "1 号线站台测试组", tag("测试", "amber"), "DTU01B", "1.2.0", "V108", "4", groupActions("test", true)],
          ["绍兴地铁样板项目", "1 号线站台正式组", tag("正式", "blue"), "DTU01B", "1.2.0", "V100", "28", groupActions("formal", true)],
          ["机房环境监控", "机房温湿度测试组", tag("测试", "amber"), "DTU01B", "1.2.0", "V27", "3", groupActions("test", false)]
        ])}
      `,

      members: () => `
        ${toolbar("成员权限", "用户管理员创建子用户，并分配项目、设备组和动作权限。", ["新建子用户"])}
        ${table(["账号", "显示名", "项目", "设备组", "权限", "状态", "操作"], [
          ["op_zhang", "张工", "绍兴地铁样板项目", "1 号线站台测试组", "查看 / 设备同步", tag("启用", "green"), actions(["编辑", "停用"])],
          ["dev_li", "李工", "绍兴地铁样板项目", "测试组", "配置编辑 / 发布", tag("启用", "green"), actions(["编辑", "停用"])]
        ])}
      `,

      tenantTasks: () => tasksPage("本用户"),
      childHome: () => `
        <div class="bar"><div><h2>我的工作台</h2><p>显示授权范围内可访问的设备。</p></div></div>
        <div class="notice" style="margin-bottom:14px;"><strong>权限受限</strong><span>当前子用户只能访问已授权的项目和设备组，但可访问设备应完整展示。</span></div>
        ${filters(["搜索设备编号", "项目", "设备组", "全部在线状态"])}
        ${table(["设备编号", "产品型号", "项目", "设备组", "在线", "固件版本", "配置版本", "最后心跳", "操作"], [
          [mono("DTU01B-0093"), "DTU01B", "绍兴地铁样板项目", "1 号线站台测试组", tag("在线", "green"), "1.2.0", "V108", "1 分钟前", actions(["查看", "同步"])],
          [mono("DTU01B-0096"), "DTU01B", "绍兴地铁样板项目", "1 号线站台测试组", tag("离线", "red"), "1.2.0", "V105", "2 小时前", actions(["查看"])],
          [mono("DTU01B-0097"), "DTU01B", "绍兴地铁样板项目", "1 号线站台测试组", tag("在线", "green"), "1.2.0", "V108", "5 分钟前", actions(["查看", "同步"])]
        ])}
      `,
      childProjects: () => `
        ${toolbar("项目管理", "子用户只看到已授权的项目和设备组。", [])}
        ${table(["项目", "设备组", "组类型", "产品型号", "组配置版本", "权限", "操作"], [
          ["绍兴地铁样板项目", "1 号线站台测试组", tag("测试", "amber"), "DTU01B", "V108", "查看 / 同步", actions(["进入设备组"], "openGroup")]
        ])}
      `,
      childDevices: () => `
        ${toolbar("设备配置", "只显示可访问设备组内设备，按权限置灰不可用按钮。", [])}
        ${filters(["搜索设备编号", "项目", "设备组", "全部在线状态"])}
        ${table(["设备编号", "设备组", "在线", "设备配置版本", "目标配置版本", "操作"], [
          [mono("DTU01B-0093"), "1 号线站台测试组", tag("在线", "green"), "V108", "V108", actions(["查看", "同步"])],
          [mono("DTU01B-0096"), "1 号线站台测试组", tag("离线", "red"), "V105", "V108", actions(["查看"])]
        ])}
      `,
      childTasks: () => tasksPage("授权范围")
    };

    function metric(label, value, note, color, tagText) {
      return `<div class="metric"><div class="metric-label"><span>${label}</span>${tag(tagText, color)}</div><div><div class="metric-value">${value}</div><div class="note">${note}</div></div></div>`;
    }

    function toolbar(title, desc, btns, actions) {
      const actionNames = actions || [];
      return `<div class="bar"><div><h2>${title}</h2><p>${desc}</p></div><div class="actions">${btns.map((name, index) => `<button class="btn ${index === btns.length - 1 ? "primary" : ""}" data-action="${actionNames[index] || ""}" type="button">${name}</button>`).join("")}</div></div>`;
    }

    function filters(placeholders) {
      return `<div class="filters">${placeholders.map((text, index) => index === 0 ? `<input class="input" placeholder="${text}">` : `<select class="select"><option>${text}</option></select>`).join("")}<button class="btn" type="button">查询</button></div>`;
    }

    function table(headers, rows) {
      return `<div class="panel"><div class="table-wrap"><table><thead><tr>${headers.map((h) => `<th>${h}</th>`).join("")}</tr></thead><tbody>${rows.map((row) => `<tr>${row.map((cell) => `<td>${cell}</td>`).join("")}</tr>`).join("")}</tbody></table></div></div>`;
    }

    function actions(names, action) {
      return `<div class="row-actions">${names.map((name) => `<button class="link" data-action="${action || ""}" type="button">${name}</button>`).join("")}</div>`;
    }

    function groupActions(type, canWrite) {
      const disabled = (name) => `<button class="link disabled" type="button" aria-disabled="true">${name}</button>`;
      const active = (name, action) => `<button class="link" data-action="${action || ""}" type="button">${name}</button>`;
      const items = [
        active("配置", "openGroupConfig"),
        canWrite ? active("编辑") : disabled("编辑"),
        active("列表", "openGroup"),
        canWrite ? active("删除") : disabled("删除"),
        type === "test" && canWrite ? active("关联") : disabled("关联"),
        type === "test" && canWrite ? active("发布") : disabled("发布")
      ];
      return `<div class="row-actions">${items.join("")}</div>`;
    }

    function mono(text) {
      return `<span class="mono">${text}</span>`;
    }

    function field(label, value) {
      return `<div class="field"><label>${label}</label><input class="input" value="${value}"></div>`;
    }

    function projectCard(title, groups, devices, stale, action) {
      return `<button class="card clickable project-card-link" data-action="${action}" onclick="openPage('projectDetail')" type="button"><h3>${title}</h3><div class="mini"><span><b>${groups}</b>设备组</span><span><b>${devices}</b>设备</span><span><b>${stale}</b>未同步</span></div><div class="note">点击进入项目详情，查看该项目下设备组。</div><span class="btn primary">进入项目</span></button>`;
    }

    function crumb(items) {
      return `<div class="breadcrumb">${items.map(([page, label], index) => page ? `<button data-page-link="${page}" type="button">${label}</button>${index < items.length - 1 ? "<span>/</span>" : ""}` : `<strong>${label}</strong>`).join("")}</div>`;
    }

    function tasksPage(scope) {
      return `${toolbar("任务日志", `${scope}内设备同步、配置发布和异常任务。`, ["导出"])}
        ${table(["时间", "对象", "动作", "状态", "结果"], [
          ["2026-05-13 15:36", "1 号线站台测试组", "同步组配置 V108", tag("完成", "green"), "3 成功 / 1 离线等待"],
          ["2026-05-13 14:20", "DTU01B-0096", "配置同步", tag("等待", "amber"), "设备离线，等待上线"],
          ["2026-05-12 18:05", "1 号线站台正式组", "发布配置 V100", tag("完成", "green"), "28 台任务已创建"]
        ])}`;
    }

    function bindContentActions() {
      content.querySelectorAll("[data-action]").forEach((button) => {
        const action = button.dataset.action;
        if (action === "openProject") button.addEventListener("click", () => openPage("projectDetail"));
        if (action === "openGroup") button.addEventListener("click", () => openPage("groupDetail"));
        if (action === "openGroupConfig") button.addEventListener("click", () => openPage("groupConfig"));
        if (action === "openAddDevice") button.addEventListener("click", () => modal.classList.add("open"));
        if (action === "openProductModel") button.addEventListener("click", () => productModal.classList.add("open"));
      });
      content.querySelectorAll(".project-card-link").forEach((button) => {
        button.addEventListener("keydown", (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            openPage("projectDetail");
          }
        });
      });
      content.querySelectorAll("[data-page-link]").forEach((button) => {
        button.addEventListener("click", () => openPage(button.dataset.pageLink));
      });
      content.querySelectorAll(".tab").forEach((tab) => {
        tab.addEventListener("click", () => {
          tab.parentElement.querySelectorAll(".tab").forEach((item) => item.classList.remove("active"));
          tab.classList.add("active");
        });
      });
    }

    document.querySelectorAll(".role-btn").forEach((button) => {
      button.addEventListener("click", () => switchRole(button.dataset.role));
    });

    document.querySelectorAll("[data-close-modal]").forEach((button) => {
      button.addEventListener("click", () => modal.classList.remove("open"));
    });

    document.querySelectorAll("[data-close-product-modal]").forEach((button) => {
      button.addEventListener("click", () => productModal.classList.remove("open"));
    });

    modal.addEventListener("click", (event) => {
      if (event.target === modal) modal.classList.remove("open");
    });

    productModal.addEventListener("click", (event) => {
      if (event.target === productModal) productModal.classList.remove("open");
    });

    openPage(currentPage);
