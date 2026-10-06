// 发布者节点：以固定频率 f 向话题 topic 发布递增计数。
// 发布周期 T = 1 / f，本实现取 f = 10 Hz，故 T = 100 ms。
#include <chrono>
#include <memory>
#include <string>

#include "rclcpp/rclcpp.hpp"
#include "std_msgs/msg/string.hpp"

using namespace std::chrono_literals;

class PublisherNode : public rclcpp::Node {
  public:
    explicit PublisherNode() : Node("publisher_node"), count_(0) {
        publisher_ = this->create_publisher<std_msgs::msg::String>("topic", 10);
        // 定时器周期即发布周期 T = 100 ms。
        timer_ = this->create_wall_timer(100ms, std::bind(&PublisherNode::timer_callback, this));
    }

  private:
    void timer_callback() {
        auto message = std_msgs::msg::String();
        message.data = "count: " + std::to_string(count_++);
        RCLCPP_INFO(this->get_logger(), "Publishing: '%s'", message.data.c_str());
        publisher_->publish(message);
    }

    rclcpp::TimerBase::SharedPtr timer_;
    rclcpp::Publisher<std_msgs::msg::String>::SharedPtr publisher_;
    size_t count_;
};

int main(int argc, char* argv[]) {
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<PublisherNode>());
    rclcpp::shutdown();
    return 0;
}
