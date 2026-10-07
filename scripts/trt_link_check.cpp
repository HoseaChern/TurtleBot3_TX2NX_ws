// 最小 TensorRT 链接自检: 创建 IRuntime 并打印 TensorRT 库版本.
// 用途: 验证 Jazzy 容器内 gcc 能编译并链接宿主挂载的 TensorRT 8.2.
#include <cstdio>

#include <NvInfer.h>

class Logger : public nvinfer1::ILogger {
  public:
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            printf("[TRT] %s\n", msg);
        }
    }
};

int main() {
    Logger logger;
    nvinfer1::IRuntime* runtime = nvinfer1::createInferRuntime(logger);
    if (runtime == nullptr) {
        printf("createInferRuntime failed\n");
        return 1;
    }
    printf("TensorRT version: %d\n", getInferLibVersion());
    runtime->destroy();
    return 0;
}
