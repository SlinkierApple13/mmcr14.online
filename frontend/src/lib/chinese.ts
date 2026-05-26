const DIGITS = ['零', '一', '二', '三', '四', '五', '六', '七', '八', '九']
const TENS = ['', '十', '百', '千']

function toChineseUnder10000(n: number): string {
  let result = ''
  let remaining = n
  let hasNonZero = false

  for (let i = 3; i >= 0; i--) {
    const divisor = Math.pow(10, i)
    const digit = Math.floor(remaining / divisor)
    remaining %= divisor

    if (digit > 0) {
      if (i === 1 && digit === 1 && !hasNonZero) {
        result += '十'
      } else {
        result += DIGITS[digit] + TENS[i]
      }
      hasNonZero = true
    } else if (hasNonZero && remaining > 0 && i > 0) {
      const nextDivisor = Math.pow(10, i - 1)
      const nextDigit = remaining >= nextDivisor ? Math.floor(remaining / nextDivisor) : 0
      if (nextDigit > 0) {
        result += '零'
      }
    }
  }

  return result || '零'
}

export function toChineseNumeral(n: number): string {
  if (n === 0) return '零'
  if (n < 0) return '负' + toChineseUnder10000(-n)

  const yi = Math.floor(n / 100000000)
  const wan = Math.floor((n % 100000000) / 10000)
  const rest = n % 10000

  let result = ''
  if (yi > 0) {
    result += toChineseUnder10000(yi) + '亿'
  }
  if (wan > 0) {
    if (yi > 0 && wan < 1000) result += '零'
    result += toChineseUnder10000(wan) + '万'
  }
  if (rest > 0) {
    if ((yi > 0 || wan > 0) && rest < 1000) result += '零'
    result += toChineseUnder10000(rest)
  }

  return result
}

export function rankToChinese(level: number): string {
  if (level === 0) return '初段'
  return toChineseNumeral(level) + '段'
}
