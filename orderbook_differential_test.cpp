#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <iomanip>
#include <map>
#include <deque>
#include <random>
#include <cstdio>
#define private public
#define main orderbook_main
#include "orderbook_fixed.cpp"
#undef main
#include <map>
#include <deque>
#include <random>
#include <cassert>
struct Ref {
  struct O{uint64_t id; int64_t q;};
  std::map<int64_t,std::deque<O>> bids, asks; // bids keyed by price
  std::vector<Trade> trades;
  std::map<uint64_t,std::pair<Side,int64_t>> where;
  int64_t match(uint64_t id, Side s, int64_t lim, int64_t q){
    if(s==Side::Buy){
      while(q>0 && !asks.empty() && asks.begin()->first<=lim){
        auto& dq=asks.begin()->second; auto p=asks.begin()->first;
        while(q>0&&!dq.empty()){auto&r=dq.front();auto t=std::min(q,r.q);trades.push_back({id,r.id,p,t});q-=t;r.q-=t;if(r.q==0){where.erase(r.id);dq.pop_front();}}
        if(dq.empty())asks.erase(asks.begin());
      }
    } else {
      while(q>0 && !bids.empty() && bids.rbegin()->first>=lim){
        auto it=std::prev(bids.end());auto&dq=it->second;auto p=it->first;
        while(q>0&&!dq.empty()){auto&r=dq.front();auto t=std::min(q,r.q);trades.push_back({r.id,id,p,t});q-=t;r.q-=t;if(r.q==0){where.erase(r.id);dq.pop_front();}}
        if(dq.empty())bids.erase(it);
      }
    }
    return q;
  }
  int64_t avail(Side s,int64_t lim){int64_t a=0; if(s==Side::Buy){for(auto&[p,dq]:asks){if(p>lim)break;for(auto&o:dq)a+=o.q;}} else {for(auto it=bids.rbegin();it!=bids.rend();++it){if(it->first<lim)break;for(auto&o:it->second)a+=o.q;}} return a;}
  void add(uint64_t id,Side s,OrderType t,int64_t p,int64_t q){
    int64_t eff=p; if(t==OrderType::Market) eff=(s==Side::Buy)?20000:1;
    if(t==OrderType::FOK && avail(s,eff)<q) return;
    auto rem=match(id,s,eff,q);
    if(rem>0&&t==OrderType::Limit){ (s==Side::Buy?bids:asks)[p].push_back({id,rem}); where[id]={s,p}; }
  }
  void cancel(uint64_t id){auto it=where.find(id);if(it==where.end())return;auto [s,p]=it->second;auto&m=(s==Side::Buy?bids:asks);auto&dq=m[p];for(auto i=dq.begin();i!=dq.end();++i)if(i->id==id){dq.erase(i);break;}if(dq.empty())m.erase(p);where.erase(it);}
};
int main(){
  std::mt19937_64 rng(42);
  for(int round=0;round<200;++round){
    OrderBook b(1,20000,50000); Ref r; uint64_t id=1; std::vector<uint64_t> live;
    for(int step=0;step<2000;++step){
      int k=rng()%10;
      if(k<7){
        Side s=(rng()&1)?Side::Buy:Side::Sell;
        int tt=rng()%10; OrderType t= tt<6?OrderType::Limit: tt<7?OrderType::Market: tt<9?OrderType::IOC:OrderType::FOK;
        int64_t p=9900+rng()%200, q=1+rng()%20;
        b.addOrder(id,s,t,p,q); r.add(id,s,t,p,q); live.push_back(id); ++id;
      } else if(!live.empty()){
        uint64_t c=live[rng()%live.size()]; b.cancelOrder(c); r.cancel(c);
      }
    }
    if(b.trades.size()!=r.trades.size()){printf("round %d: trade count %zu vs %zu\n",round,b.trades.size(),r.trades.size());return 1;}
    for(size_t i=0;i<b.trades.size();++i){auto&x=b.trades[i];auto&y=r.trades[i];
      if(x.buyOrderId!=y.buyOrderId||x.sellOrderId!=y.sellOrderId||x.price!=y.price||x.quantity!=y.quantity){printf("round %d: trade %zu differs\n",round,i);return 1;}}
    for(auto&[p,dq]:r.bids){int64_t q=0;for(auto&o:dq)q+=o.q;if(b.bids[p-1].totalQuantity!=q){printf("round %d: bid level %ld differs\n",round,(long)p);return 1;}}
    for(auto&[p,dq]:r.asks){int64_t q=0;for(auto&o:dq)q+=o.q;if(b.asks[p-1].totalQuantity!=q){printf("round %d: ask level %ld differs\n",round,(long)p);return 1;}}
  }
  printf("all 200 rounds match\n");
  return 0;
}
