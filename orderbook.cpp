#include <iostream>
#include <vector>
#include <cstdint>
#include <algorithm>
#include <limits>
#include <iomanip>

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Limit, Market, IOC, FOK, Stop, StopLimit };

using Price = int64_t;      // Stored in integer pence (e.g., 10150 = £101.50)
using Quantity = int64_t;
using OrderId = uint64_t;
using Index = int32_t;

constexpr Index INVALID_IDX = -1;
constexpr OrderId EMPTY_ID = 0;

struct OrderNode {
    OrderId id = EMPTY_ID;
    Price price = 0;        // Limit price (used immediately, or after StopLimit triggers)
    Price stopPrice = 0;    // Trigger price for Stop / StopLimit
    Quantity quantity = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    bool isWaitingStop = false;

    Index prev = INVALID_IDX;
    Index next = INVALID_IDX;
};

struct PriceLevel {
    Index head = INVALID_IDX;
    Index tail = INVALID_IDX;
    Quantity totalQuantity = 0;
};

struct Trade {
    OrderId buyOrderId;
    OrderId sellOrderId;
    Price price;            // Executed price in pence
    Quantity quantity;
};

// Open-addressed linear-probing hash table (zero heap allocations after init)
class FlatOrderMap {
private:
    struct Entry {
        OrderId key = EMPTY_ID;
        Index value = INVALID_IDX;
        bool deleted = false;
    };
    std::vector<Entry> table;
    size_t mask;

    size_t hash(OrderId key) const {
        key ^= key >> 33;
        key *= 0xff51afd7ed558ccdULL;
        key ^= key >> 33;
        return static_cast<size_t>(key) & mask;
    }

public:
    explicit FlatOrderMap(size_t capacityPowerOfTwo = 262144) {
        table.resize(capacityPowerOfTwo);
        mask = capacityPowerOfTwo - 1;
    }

    void insert(OrderId key, Index value) {
        size_t idx = hash(key);
        while (table[idx].key != EMPTY_ID && !table[idx].deleted && table[idx].key != key) {
            idx = (idx + 1) & mask;
        }
        table[idx] = {key, value, false};
    }

    Index find(OrderId key) const {
        if (key == EMPTY_ID) return INVALID_IDX;
        size_t idx = hash(key);
        size_t start = idx;
        while (table[idx].key != EMPTY_ID) {
            if (!table[idx].deleted && table[idx].key == key) {
                return table[idx].value;
            }
            idx = (idx + 1) & mask;
            if (idx == start) break;
        }
        return INVALID_IDX;
    }

    void erase(OrderId key) {
        size_t idx = hash(key);
        size_t start = idx;
        while (table[idx].key != EMPTY_ID) {
            if (!table[idx].deleted && table[idx].key == key) {
                table[idx].deleted = true;
                return;
            }
            idx = (idx + 1) & mask;
            if (idx == start) break;
        }
    }
};

class OrderBook {
private:
    Price minPrice;
    Price maxPrice;
    Index numLevels;

    // Flat arrays indexed directly by (price - minPrice)
    std::vector<PriceLevel> bids;
    std::vector<PriceLevel> asks;
    std::vector<PriceLevel> buyStops;
    std::vector<PriceLevel> sellStops;

    // Track best active indices for O(1) top-of-book access
    Index bestBidIdx = INVALID_IDX;
    Index bestAskIdx = INVALID_IDX;
    Index lowestBuyStopIdx = INVALID_IDX;
    Index highestSellStopIdx = INVALID_IDX;

    // Contiguous Order Memory Pool
    std::vector<OrderNode> pool;
    Index freeHead = INVALID_IDX;

    // Flat O(1) ID -> Pool Index lookup
    FlatOrderMap idMap;

    std::vector<Trade> trades;

    Index priceToIdx(Price p) const {
        if (p < minPrice || p > maxPrice) return INVALID_IDX;
        return static_cast<Index>(p - minPrice);
    }

    Price idxToPrice(Index idx) const {
        return minPrice + idx;
    }

public:
    // Define price band in pence (e.g., 1p to 50000p = £0.01 to £500.00)
    OrderBook(Price minPence = 1, Price maxPence = 50000, size_t maxOrders = 100000, size_t mapCap = 262144)
        : minPrice(minPence), maxPrice(maxPence), idMap(mapCap) {
        numLevels = static_cast<Index>(maxPrice - minPrice + 1);
        bids.resize(numLevels);
        asks.resize(numLevels);
        buyStops.resize(numLevels);
        sellStops.resize(numLevels);

        pool.resize(maxOrders);
        for (size_t i = 0; i < maxOrders - 1; ++i) {
            pool[i].next = static_cast<Index>(i + 1);
        }
        pool[maxOrders - 1].next = INVALID_IDX;
        freeHead = 0;
    }

    bool addOrder(OrderId id, Side side, OrderType type, Price price, Quantity quantity, Price stopPrice = 0) {
        if (id == EMPTY_ID || quantity <= 0 || idMap.find(id) != INVALID_IDX) return false;

        // 1. Park Stop & StopLimit orders in the flat stop arrays
        if (type == OrderType::Stop || type == OrderType::StopLimit) {
            Index sIdx = priceToIdx(stopPrice);
            if (sIdx == INVALID_IDX) return false;
            if (type == OrderType::StopLimit && priceToIdx(price) == INVALID_IDX) return false;

            Index nodeIdx = allocateNode();
            if (nodeIdx == INVALID_IDX) return false;

            pool[nodeIdx] = {id, price, stopPrice, quantity, side, type, true, INVALID_IDX, INVALID_IDX};
            idMap.insert(id, nodeIdx);

            if (side == Side::Buy) {
                appendToLevel(buyStops[sIdx], nodeIdx);
                if (lowestBuyStopIdx == INVALID_IDX || sIdx < lowestBuyStopIdx) lowestBuyStopIdx = sIdx;
            } else {
                appendToLevel(sellStops[sIdx], nodeIdx);
                if (highestSellStopIdx == INVALID_IDX || sIdx > highestSellStopIdx) highestSellStopIdx = sIdx;
            }
            return true;
        }

        // Validate limit price bounds for non-market orders
        Price effectivePrice = price;
        if (type == OrderType::Market) {
            effectivePrice = (side == Side::Buy) ? maxPrice : minPrice;
        } else if (priceToIdx(price) == INVALID_IDX) {
            return false;
        }

        // 2. Fill-or-Kill (FOK) pre-check
        if (type == OrderType::FOK) {
            if (!canFullyFill(side, effectivePrice, quantity)) return false;
        }

        // 3. Match against resting book
        Quantity remaining = matchOrder(id, side, effectivePrice, quantity);

        // 4. Rest unfilled Limit remainder on book
        if (remaining > 0 && type == OrderType::Limit) {
            Index pIdx = priceToIdx(price);
            Index nodeIdx = allocateNode();
            if (nodeIdx == INVALID_IDX) return false;

            pool[nodeIdx] = {id, price, 0, remaining, side, type, false, INVALID_IDX, INVALID_IDX};
            idMap.insert(id, nodeIdx);

            if (side == Side::Buy) {
                appendToLevel(bids[pIdx], nodeIdx);
                if (bestBidIdx == INVALID_IDX || pIdx > bestBidIdx) bestBidIdx = pIdx;
            } else {
                appendToLevel(asks[pIdx], nodeIdx);
                if (bestAskIdx == INVALID_IDX || pIdx < bestAskIdx) bestAskIdx = pIdx;
            }
        }

        return true;
    }

    bool cancelOrder(OrderId id) {
        Index nodeIdx = idMap.find(id);
        if (nodeIdx == INVALID_IDX) return false;

        OrderNode& node = pool[nodeIdx];
        if (node.isWaitingStop) {
            Index sIdx = priceToIdx(node.stopPrice);
            if (node.side == Side::Buy) {
                removeFromLevel(buyStops[sIdx], nodeIdx);
                if (buyStops[sIdx].head == INVALID_IDX && sIdx == lowestBuyStopIdx) advanceLowestBuyStop();
            } else {
                removeFromLevel(sellStops[sIdx], nodeIdx);
                if (sellStops[sIdx].head == INVALID_IDX && sIdx == highestSellStopIdx) advanceHighestSellStop();
            }
        } else {
            Index pIdx = priceToIdx(node.price);
            if (node.side == Side::Buy) {
                removeFromLevel(bids[pIdx], nodeIdx);
                if (bids[pIdx].head == INVALID_IDX && pIdx == bestBidIdx) advanceBestBid();
            } else {
                removeFromLevel(asks[pIdx], nodeIdx);
                if (asks[pIdx].head == INVALID_IDX && pIdx == bestAskIdx) advanceBestAsk();
            }
        }

        idMap.erase(id);
        freeNode(nodeIdx);
        return true;
    }

    bool modifyOrder(OrderId id, Price newPrice, Quantity newQuantity) {
        if (newQuantity <= 0) return cancelOrder(id);

        Index nodeIdx = idMap.find(id);
        if (nodeIdx == INVALID_IDX) return false;

        OrderNode& node = pool[nodeIdx];
        if (node.isWaitingStop) return false;

        // Preserve FIFO priority when reducing size at same price
        if (node.price == newPrice && newQuantity <= node.quantity) {
            Quantity diff = node.quantity - newQuantity;
            node.quantity = newQuantity;
            Index pIdx = priceToIdx(newPrice);
            if (node.side == Side::Buy) bids[pIdx].totalQuantity -= diff;
            else asks[pIdx].totalQuantity -= diff;
            return true;
        }

        Side side = node.side;
        OrderType type = node.type;
        cancelOrder(id);
        return addOrder(id, side, type, newPrice, newQuantity);
    }

    void printBook() const {
        std::cout << "\n UK ORDER BOOK (GBP) \n";
        std::cout << "ASKS (Sell):\n";
        if (bestAskIdx != INVALID_IDX) {
            for (Index i = numLevels - 1; i >= bestAskIdx; --i) {
                if (asks[i].totalQuantity > 0) {
                    std::cout << "  £" << std::fixed << std::setprecision(2) << (idxToPrice(i) / 100.0)
                              << " (" << idxToPrice(i) << "p) | Qty: " << asks[i].totalQuantity << "\n";
                }
            }
        }
        std::cout << "---------------------------\n";
        std::cout << "BIDS (Buy):\n";
        if (bestBidIdx != INVALID_IDX) {
            for (Index i = bestBidIdx; i >= 0; --i) {
                if (bids[i].totalQuantity > 0) {
                    std::cout << "  £" << std::fixed << std::setprecision(2) << (idxToPrice(i) / 100.0)
                    << " (" << idxToPrice(i) << "p) | Qty: " << bids[i].totalQuantity << "\n";
                }
            }
        }
        std::cout << "---------------------------\n\n";
    }
    void printTrades() const {
        std::cout << " EXECUTED TRADES \n";
            for (const auto& t : trades) {
            std::cout << "Buy #" << t.buyOrderId << " <-> Sell #" << t.sellOrderId
            << " | Qty: " << t.quantity << " @ £" << std::fixed << std::setprecision(2)
            << (t.price / 100.0) << " (" << t.price << "p)\n";
        }
        std::cout << "-----------------------\n";
    }
private:
    Index allocateNode() {
        if (freeHead == INVALID_IDX) return INVALID_IDX;
        Index idx = freeHead;
        freeHead = pool[idx].next;
        return idx;
    }
    void freeNode(Index idx) {
        pool[idx].next = freeHead;
        freeHead = idx;
    }
    void appendToLevel(PriceLevel& level, Index idx) {
        OrderNode& node = pool[idx];
        node.prev = level.tail;
        node.next = INVALID_IDX;
        if (level.tail != INVALID_IDX) pool[level.tail].next = idx;
        else level.head = idx;
        level.tail = idx;
        level.totalQuantity += node.quantity;
    }
    void removeFromLevel(PriceLevel& level, Index idx) {
        OrderNode& node = pool[idx];
        if (node.prev != INVALID_IDX) pool[node.prev].next = node.next;
        else level.head = node.next;
        if (node.next != INVALID_IDX) pool[node.next].prev = node.prev;
        else level.tail = node.prev;
        level.totalQuantity -= node.quantity;
    }
    void advanceBestBid() {
        while (bestBidIdx >= 0 && bids[bestBidIdx].head == INVALID_IDX) --bestBidIdx;
        if (bestBidIdx < 0) bestBidIdx = INVALID_IDX;
    }
    void advanceBestAsk() {
        while (bestAskIdx < numLevels && asks[bestAskIdx].head == INVALID_IDX) ++bestAskIdx;
        if (bestAskIdx >= numLevels) bestAskIdx = INVALID_IDX;
    }
    void advanceLowestBuyStop() {
        while (lowestBuyStopIdx < numLevels && buyStops[lowestBuyStopIdx].head == INVALID_IDX) ++lowestBuyStopIdx;
        if (lowestBuyStopIdx >= numLevels) lowestBuyStopIdx = INVALID_IDX;
    }
    void advanceHighestSellStop() {
        while (highestSellStopIdx >= 0 && sellStops[highestSellStopIdx].head == INVALID_IDX) --highestSellStopIdx;
        if (highestSellStopIdx < 0) highestSellStopIdx = INVALID_IDX;
    }
    bool canFullyFill(Side side, Price limitPrice, Quantity requiredQty) const {
        Quantity available = 0;
        if (side == Side::Buy) {
        if (bestAskIdx == INVALID_IDX) return false;
        Index limitIdx = priceToIdx(limitPrice);
        for (Index i = bestAskIdx; i <= limitIdx && i < numLevels; ++i) {
            available += asks[i].totalQuantity;
            if (available >= requiredQty) return true;
        }
        } else {
            if (bestBidIdx == INVALID_IDX) return false;
            Index limitIdx = priceToIdx(limitPrice);
            for (Index i = bestBidIdx; i >= limitIdx && i >= 0; --i) {
                available += bids[i].totalQuantity;
                if (available >= requiredQty) return true;
            }
        }
        return false;
    }
    Quantity matchOrder(OrderId incomingId, Side side, Price limitPrice, Quantity qty) {
        Price lastTradePrice = 0;
        bool traded = false;
        if (side == Side::Buy) {
            Index limitIdx = priceToIdx(limitPrice);
            while (qty > 0 && bestAskIdx != INVALID_IDX && bestAskIdx <= limitIdx) {
                PriceLevel& level = asks[bestAskIdx];
                while (qty > 0 && level.head != INVALID_IDX) {
                    Index restingIdx = level.head;
                    OrderNode& resting = pool[restingIdx];
                    Quantity tradeQty = std::min(qty, resting.quantity);
                    trades.push_back({incomingId, resting.id, resting.price, tradeQty});
                    lastTradePrice = resting.price;
                    traded = true;
                    qty -= tradeQty;
                    resting.quantity -= tradeQty;
                    level.totalQuantity -= tradeQty;
                    if (resting.quantity == 0) {
                        idMap.erase(resting.id);
                        level.head = resting.next;
                        if (level.head != INVALID_IDX) pool[level.head].prev = INVALID_IDX;
                        else level.tail = INVALID_IDX;
                        freeNode(restingIdx);
                    }
                }
                if (level.head == INVALID_IDX) advanceBestAsk();
            }
        } else {
            Index limitIdx = priceToIdx(limitPrice);
            while (qty > 0 && bestBidIdx != INVALID_IDX && bestBidIdx >= limitIdx) {
                PriceLevel& level = bids[bestBidIdx];
                while (qty > 0 && level.head != INVALID_IDX) {
                    Index restingIdx = level.head;
                    OrderNode& resting = pool[restingIdx];
                    Quantity tradeQty = std::min(qty, resting.quantity);
                    trades.push_back({resting.id, incomingId, resting.price, tradeQty});
                    lastTradePrice = resting.price;
                    traded = true;
                    qty -= tradeQty;
                    resting.quantity -= tradeQty;
                    level.totalQuantity -= tradeQty;
                    if (resting.quantity == 0) {
                        idMap.erase(resting.id);
                        level.head = resting.next;
                        if (level.head != INVALID_IDX) pool[level.head].prev = INVALID_IDX;
                        else level.tail = INVALID_IDX;
                        freeNode(restingIdx);
                    }
                }
                if (level.head == INVALID_IDX) advanceBestBid();
            }
        }
        if (traded) triggerStops(lastTradePrice);
        return qty;
    }
    void triggerStops(Price tradePrice) {
        Index tradeIdx = priceToIdx(tradePrice);
        if (tradeIdx == INVALID_IDX) return;
        // Collect triggered orders on stack/local buffer before re-injecting
        std::vector<OrderNode> triggered;
        // 1. Trigger Buy Stops where stopPrice <= tradePrice
        while (lowestBuyStopIdx != INVALID_IDX && lowestBuyStopIdx <= tradeIdx) {
            PriceLevel& level = buyStops[lowestBuyStopIdx];
            Index curr = level.head;
            while (curr != INVALID_IDX) {
                Index next = pool[curr].next;
                triggered.push_back(pool[curr]);
                idMap.erase(pool[curr].id);
                freeNode(curr);
                curr = next;
            }
            level = {INVALID_IDX, INVALID_IDX, 0};
            advanceLowestBuyStop();
        }
        // 2. Trigger Sell Stops where stopPrice >= tradePrice
        while (highestSellStopIdx != INVALID_IDX && highestSellStopIdx >= tradeIdx) {
            PriceLevel& level = sellStops[highestSellStopIdx];
            Index curr = level.head;
            while (curr != INVALID_IDX) {
                Index next = pool[curr].next;
                triggered.push_back(pool[curr]);
                idMap.erase(pool[curr].id);
                freeNode(curr);
                curr = next;
            }
            level = {INVALID_IDX, INVALID_IDX, 0};
            advanceHighestSellStop();
        }
        // 3. Re-inject triggered orders as Market (for Stop) or Limit (for StopLimit)
        for (const auto& s : triggered) {
            OrderType nextType = (s.type == OrderType::StopLimit) ? OrderType::Limit : OrderType::Market;
            addOrder(s.id, s.side, nextType, s.price, s.quantity);
        }
    }
};

int main() {
    // Price band: 1p (£0.01) to 20,000p (£200.00)
    OrderBook book(1, 20000, 10000);
    // 1. Resting Limit Orders (Prices in integer pence)
    book.addOrder(1, Side::Sell, OrderType::Limit, 10250, 10); // £102.50
    book.addOrder(2, Side::Sell, OrderType::Limit, 10100, 15); // £101.00
    book.addOrder(3, Side::Buy,  OrderType::Limit,  9950, 20); // £99.50
    book.addOrder(4, Side::Buy,  OrderType::Limit,  9800, 10); // £98.00
    // 2. Buy Stop-Limit: Triggers at £102.00 (10200p), places Limit Buy at £102.50 (10250p)
    book.addOrder(5, Side::Buy, OrderType::StopLimit, 10250, 8, 10200);
    // 3. Modify Order #3 (reduce qty from 20 to 12 -> preserves FIFO priority)
    book.modifyOrder(3, 9950, 12);
    // 4. IOC Buy Order: Sweeps 15 @ £101.00 and 5 @ £102.50 (triggers Stop-Limit #5!)
    book.addOrder(6, Side::Buy, OrderType::IOC, 10250, 20);
    book.printTrades();
    book.printBook();
    return 0;
}
