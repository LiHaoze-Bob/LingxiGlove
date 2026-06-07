import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "灵犀手套 LingxiGlove",
  description: "手语翻译手套实时展示端",
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="zh-CN">
      <body>
        {children}
      </body>
    </html>
  );
}
