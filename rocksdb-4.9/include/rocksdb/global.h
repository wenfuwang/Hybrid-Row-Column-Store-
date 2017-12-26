#ifndef GLOBAL_H_
#define GLOBAL_H_

#include <iostream>
#include <chrono>
#include <deque>
using namespace std::chrono;
using namespace std;

extern steady_clock::time_point t1;
extern steady_clock::time_point t2;
extern steady_clock::time_point t3;
extern steady_clock::time_point t11;
extern steady_clock::time_point t12;
extern steady_clock::time_point t13;
extern steady_clock::time_point t14;
extern steady_clock::time_point t15;
extern steady_clock::time_point t16;
extern duration<double> diff1;
extern duration<double> diff2;
extern duration<double> diff3;
extern bool flag;

struct Query {
  Query(int type, uint64_t level_seq): type_(type), level_seq_(level_seq) {}
  int type_;
  uint64_t level_seq_;
};

class Advisor {
public:
  Advisor() {}
  void Push(Query& q) { buf.push_back(q); }
  void Pop() { buf.pop_front(); }
  int CalculateK(vector<double>& costs) {
    steady_clock::time_point t7 = steady_clock::now();
    double cost = numeric_limits<double>::max();
    int res = -1;
    for (uint i=0; i<K; ++i) {
      double tmp = 0;
      for (auto q : buf) {
        if (!q.type_) {
          tmp += q.level_seq_ < i ? oltp_row : oltp_col;
        } else {
          uint level = numeric_limits<unsigned int>::max();
          if (q.level_seq_ <= bs[0] && q.level_seq_ > bs[1]) level = 0;
          else if (q.level_seq_ <= bs[1] && q.level_seq_ >= bs[2]) level = 1;
          else if (q.level_seq_ < bs[2] && q.level_seq_ >= bs[3]) level = 2;
          tmp += level >= i ? q.level_seq_ * olap_col[q.type_] :
                              (q.level_seq_-bs[i]) * olap_row + bs[i] * olap_col[q.type_];
        }
      }
      costs.push_back(tmp);
      if (tmp < cost) {
        cost = tmp;
        res = i;
      }
    }
    steady_clock::time_point t8 = steady_clock::now();
    cout << "Advisor costs: " << duration_cast<duration<double>>(t8 - t7).count() << endl;
    return res;
  }
  void Print() {
    for (auto q : buf) {
      cout << q.type_ << ", " << q.level_seq_ << endl;
    }
  }

private:
  deque<Query> buf;
  uint K = 4;
  double oltp_row = 0.0086;
  double oltp_col = 0.0954;
  double olap_row = 6.5e-7;
  vector<double> olap_col = {0, 1.85e-7, 2.93e-7, 4e-7};  // delta 1.08
  vector<uint64_t> bs = {60000000, 50000000, 40000000, 0};
};

extern Advisor advisor;

#endif /* GLOBAL_H_ */
