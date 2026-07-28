import fandolKaiUrl from '../fonts/fandol-kai-regular.woff2'
import fandolFangUrl from '../fonts/fandol-fang-regular.woff2'
import latinModernUrl from '../fonts/latinmodern-math.woff2'

let fontsInjected = false

export function loadMissingFonts(): void {
  if (fontsInjected) {
    return
  }

  const fonts = [
    {
      name: 'GameKai',
      localNames: [
        'KaiTi',
        'KaiTi_GB2312',
        'SimKai',
        'STKaiti',
        'Kaiti SC',
        'Kaiti TC',
        '楷体',
        '楷体_GB2312',
        '標楷體',
        'BiauKai',
        'DFKai-SB',
        'FandolKai',
      ],
      url: fandolKaiUrl,
      format: 'woff2',
    },
    {
      name: 'GameFangsong',
      localNames: [
        'FangSong',
        'FangSong_GB2312',
        'SimFang',
        'STFangsong',
        'Fangsong SC',
        'Fangsong TC',
        '仿宋',
        '仿宋_GB2312',
        '华文仿宋',
        'FandolFang R',
      ],
      url: fandolFangUrl,
      format: 'woff2',
    },
    {
      name: 'CmuSerif',
      localNames: ['CMU Serif', 'CMU Serif Regular', 'Latin Modern Math'],
      url: latinModernUrl,
      format: 'woff2',
    },
  ]

  const rules = fonts.map((font) => {
    const localSrc = font.localNames.map((name) => `local('${name}')`).join(', ')
    const src = `${localSrc}, url('${font.url}') format('${font.format}')`
    return `
@font-face {
  font-family: '${font.name}';
  src: ${src};
  font-weight: normal;
  font-style: normal;
  font-display: swap;
}`
  })

  const styleTag = document.createElement('style')
  styleTag.textContent = rules.join('\n')
  document.head.appendChild(styleTag)
  fontsInjected = true
}

export async function waitForGameFonts(): Promise<void> {
  loadMissingFonts()
  await Promise.allSettled([
    document.fonts.load('300px GameKai'),
    document.fonts.load('300px GameFangsong'),
    document.fonts.load('300px CmuSerif'),
  ])
}