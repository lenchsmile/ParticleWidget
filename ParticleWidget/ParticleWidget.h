#pragma once

#include <QOpenGLWidget>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QElapsedTimer>
#include <QTimer>
#include <QMatrix4x4>
#include <QRandomGenerator>
#include <QDebug>

struct Particle
{
	float px,py,pz,pw; // Position(xyz), Life(w)
	float vx,vy,vz,vw; // Velocity(xyz), Delay(w)
};

class ParticleWidget : public QOpenGLWidget,protected QOpenGLExtraFunctions
{
	Q_OBJECT

public:
	explicit ParticleWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent),ssbo(0),vao(0)
	{
		QSurfaceFormat fmt;
		fmt.setVersion(4,6);
		fmt.setProfile(QSurfaceFormat::CoreProfile);
		setFormat(fmt);

		timer.start();
		QTimer* refreshTimer = new QTimer(this);
		connect(refreshTimer,&QTimer::timeout,this,[this]()
		{
			update();
		});
		refreshTimer->start(16);
	}

	~ParticleWidget()
	{
		makeCurrent();
		if(ssbo) glDeleteBuffers(1,&ssbo);
		if(vao) glDeleteVertexArrays(1,&vao);
	}

protected:
	void initializeGL() override
	{
		initializeOpenGLFunctions();
		qDebug() << "Current Renderer:" << (const char*)glGetString(GL_RENDERER);

		// 1. 必须创建一个 VAO，即使我们不通过它传递数据
		glGenVertexArrays(1,&vao);
		glBindVertexArray(vao);

		// 2. Compute Shader
		const char* compSrc = R"(
           #version 460 core

// 定义本地工作组大小，256 在大多数现代 GPU 上性能最优
layout(local_size_x = 256) in;

// 粒子结构体：必须与 C++ 端保持严格的 32 字节对齐
struct Particle {
    vec4 pos; // xyz: 当前位置, w: 生命周期 (1.0 -> 0.0)
    vec4 vel; // xyz: 初始速度, w: 出生延迟时间 (Delay)
};

// 存储缓冲：binding = 0 对应 C++ 中的 glBindBufferBase
layout(std430, binding = 0) buffer ParticleBuffer {
    Particle particles[];
};

// 外部传入的 Uniforms
uniform float u_Time;          // 程序运行的总时间 (秒)
uniform float u_LoopDuration;  // 循环周期 (例如 5.0 秒)

void main() {
    // 获取当前粒子索引
    uint idx = gl_GlobalInvocationID.x;
    
    // 防止数组越界
    if (idx >= particles.length()) return;

    // 获取该粒子的属性
    float delay = particles[idx].vel.w;
    vec3 v0 = particles[idx].vel.xyz;

    // --- 无抖动循环核心逻辑 ---
    // t 表示粒子在其生命周期内已运行的时间
    // 使用 mod 确保当 (u_Time + delay) 超过周期时，t 立即回到 0，实现无缝循环
    float t = mod(u_Time + delay, u_LoopDuration);

    // --- 物理模拟：带阻力的喷泉模型 ---
    // 定义物理常量
    const vec3 gravity = vec3(0.0, -1.8, 0.0); // 重力加速度
    const float drag = 0.25;                  // 空气阻力系数 (越小喷得越高)

    // 基础抛物线公式: P = v0*t + 0.5*g*t^2
    // 进阶：使用空气阻力公式 P = (v0/k) * (1 - exp(-k*t)) + 重力项
    // 这能让粒子在上升初期速度很快，后期在空中停留时间更长，视觉更细腻
    float dragTerm = (1.0 - exp(-drag * t)) / drag;
    vec3 currentPos = v0 * dragTerm + 0.5 * gravity * t * t;

    // --- 生命周期计算 ---
    // life 从 1.0 (出生) 线性减小到 0.0 (消失)
    float life = 1.0 - (t / u_LoopDuration);

    // --- 增加细节：微小扰动 (可选) ---
    // 使用 sin 函数基于时间和索引给位置增加一点点晃动，模拟火焰或烟雾的不稳定性
    currentPos.x += sin(u_Time * 2.0 + float(idx)) * 0.05 * (1.0 - life);
    currentPos.z += cos(u_Time * 2.0 + float(idx)) * 0.05 * (1.0 - life);

    // --- 写回结果 ---
    particles[idx].pos = vec4(currentPos, life);
}
        )";
		computeProg.addShaderFromSourceCode(QOpenGLShader::Compute,compSrc);
		computeProg.link();

		// 3. Render Shader
		const char* vertSrc = R"(
            #version 460 core
            struct Particle { vec4 pos; vec4 vel; };
            layout(std430, binding = 0) buffer ParticleBuffer { Particle particles[]; };
            uniform mat4 u_MVP;
            out float v_Life;
            void main() {
                v_Life = particles[gl_VertexID].pos.w;
                gl_Position = u_MVP * vec4(particles[gl_VertexID].pos.xyz, 1.0);
                gl_PointSize = clamp(10.0 * v_Life, 1.0, 50.0);
            }
        )";
		const char* fragSrc = R"(
            #version 460 core

// 从 Vertex Shader 传来的生命周期：1.0 (出生) -> 0.0 (消失)
in float v_Life;

// 最终输出的颜色
out vec4 fragColor;

void main() {
    // 1. 形状裁剪：将方形点转为圆形
    // gl_PointCoord 是当前点内的坐标，范围从 (0,0) 到 (1,1)
    // 我们计算其距离中心点的长度
    vec2 circCoord = 2.0 * gl_PointCoord - 1.0; // 映射到 [-1, 1]
    float dist = dot(circCoord, circCoord);    // 计算距离平方 (x^2 + y^2)

    // 如果距离中心超过半径 1.0，则丢弃该像素，使点变圆
    if (dist > 1.0) {
        discard;
    }

    // 2. 动态颜色模拟 (黑体辐射感)
    // 出生时(v_Life=1.0)接近白色或亮黄色，消失前(v_Life=0.0)变为深红色
    vec3 hotColor = vec3(1.0, 1.0, 0.7);  // 亮白黄
    vec3 midColor = vec3(1.0, 0.4, 0.1);  // 亮橘红
    vec3 coolColor = vec3(0.6, 0.0, 0.0); // 深红暗影

    vec3 finalColor;
    if (v_Life > 0.5) {
        // 生命周期前半段：从亮白黄转为橘红
        finalColor = mix(midColor, hotColor, (v_Life - 0.5) * 2.0);
    } else {
        // 生命周期后半段：从橘红转为深红
        finalColor = mix(coolColor, midColor, v_Life * 2.0);
    }

    // 3. 边缘柔化与光晕
    // 计算边缘衰减：中心最亮，向外围平滑变淡
    float alphaMask = 1.0 - dist; 
    
    // 结合生命周期进行最终透明度计算
    // 使用 pow(alphaMask, 2.0) 可以让粒子边缘更柔和，不像硬纸片
    float alpha = pow(alphaMask, 2.0) * v_Life;

    // 4. 最终色彩输出
    fragColor = vec4(finalColor, alpha);
}
        )";
		renderProg.addShaderFromSourceCode(QOpenGLShader::Vertex,vertSrc);
		renderProg.addShaderFromSourceCode(QOpenGLShader::Fragment,fragSrc);
		renderProg.link();

		// 4. 初始化 SSBO
		const int count = 40960000;
		QVector<Particle> data(count);
		auto* gen = QRandomGenerator::global();
		for(int i = 0; i < count; ++i)
		{
			data[i].vx = (float)gen->generateDouble() * 4.0f - 2.0f;
			data[i].vy = (float)gen->generateDouble() * 5.0f + 2.0f;
			data[i].vz = (float)gen->generateDouble() * 4.0f - 2.0f;
			data[i].vw = (float)gen->generateDouble() * 5.0f; // Delay
		}

		glGenBuffers(1,&ssbo);
		glBindBuffer(GL_SHADER_STORAGE_BUFFER,ssbo);
		glBufferData(GL_SHADER_STORAGE_BUFFER,data.size() * sizeof(Particle),data.data(),GL_DYNAMIC_DRAW);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ssbo);

		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA,GL_ONE);
		glEnable(GL_PROGRAM_POINT_SIZE);
	}

	void paintGL() override
	{
		float time = timer.elapsed() / 1000.0f;

		// --- 计算步 ---
		computeProg.bind();
		computeProg.setUniformValue("u_Time",time);
		computeProg.setUniformValue("u_LoopDuration",5.0f);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ssbo);
		glDispatchCompute(25600 / 256,1,1);
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
		computeProg.release();

		// --- 渲染步 ---
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		renderProg.bind();
		QMatrix4x4 mvp;
		mvp.perspective(45.0f,(float)width() / height(),0.1f,1000.0f);
		mvp.translate(0,-2,-15);
		renderProg.setUniformValue("u_MVP",mvp);

		glBindVertexArray(vao); // Intel 驱动必须绑定 VAO
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ssbo);
		glDrawArrays(GL_POINTS,0,25600);

		renderProg.release();
	}

	void resizeGL(int w,int h) override
	{
		glViewport(0,0,w * devicePixelRatio(),h * devicePixelRatio());
	}

private:
	QOpenGLShaderProgram computeProg,renderProg;
	GLuint ssbo,vao;
	QElapsedTimer timer;
};