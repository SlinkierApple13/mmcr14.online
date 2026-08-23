#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>
#include "hand.h"

namespace mahjong {

    template<typename T, typename tag_type = uint32_t> requires std::is_arithmetic_v<T>
    class scoring_element {

    private:
        std::vector<T> (*p)(const hand&) = nullptr;
        T (*q)(const hand&) = nullptr;

    public:
        const std::string name;
        const tag_type tag;

        scoring_element(std::string name, const tag_type& tag, std::vector<T> (*f)(const hand&)) : p(f), name(name), tag(tag) {} 
        scoring_element(std::string name, std::vector<T> (*f)(const hand&)) : p(f), name(name), tag() {}  

        scoring_element(std::string name, const tag_type& tag, T (*f)(const hand&)) : q(f), name(name), tag(tag) {} 
        scoring_element(std::string name, T (*f)(const hand&)) : q(f), name(name), tag() {}

        inline std::vector<T> operator()(const hand& h) const {
            if (p) return p(h);
            if (q) return std::vector<T>{q(h)};
            return {};
        }

    };

    using verifier = std::function<bool(const hand&)>;

    template<typename R, typename T, typename element_tag_type = uint32_t, typename tag_type = uint32_t, typename detail_type = std::vector<uint8_t>> requires std::is_arithmetic_v<R> && std::is_arithmetic_v<T> && (!std::is_same_v<R, bool>)
    class scoring_system {
        
    private:
        const std::vector<scoring_element<T, element_tag_type>> elements;
        const verifier v;
    
    public:
        class result {

        private:
            bool valid;
            R score;
            tag_type t;
            detail_type d;

        public:
            result(bool valid, R score) : valid(valid), score(score) {}
            result(bool valid, R score, const tag_type& tag) : valid(valid), score(score), t(tag) {}
            result(bool valid, R score, const tag_type& tag, const detail_type& detail) : valid(valid), score(score), t(tag), d(detail) {}

            operator bool() const {
                return valid;
            }

            operator R() const {
                return valid ? score : R(0);
            }

            const tag_type& tag() const {
                return t;
            }

            const detail_type& detail() const {
                return d;
            }

        };
    
    protected:
        const std::function<result(const std::vector<std::vector<T>>&)> score_from_elements;

    public:        
        scoring_system(const std::vector<scoring_element<T, element_tag_type>>& elements, const std::function<result(const std::vector<std::vector<T>>&)>& score_from_elements, const std::function<bool(const hand&)>& ver) : elements(elements), score_from_elements(score_from_elements), v(ver) {}

        inline result operator()(const hand& h) const {
            if (!v(h)) return {false, 0};
            std::vector<std::vector<T>> results;
            for (const auto& e : elements) {
                results.push_back(e(h));
            }
            return score_from_elements(results);
        }

    };

}
