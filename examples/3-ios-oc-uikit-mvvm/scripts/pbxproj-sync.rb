#!/usr/bin/env ruby
# pbxproj-sync.rb — 用 xcodeproj gem 安全同步 pbxproj。
#
# Usage:
#   ruby pbxproj-sync.rb add <rel-path-from-project-root> [more ...]
#   ruby pbxproj-sync.rb remove <basename> [more ...]
#
# add 时：
#   • rel-path 形如 "ios-oc-mvvm/Business/Playground/Views/Foo.mm"
#   • 自动在 Group 树中按目录结构找到/创建对应 group
#   • 根据扩展名决定 lastKnownFileType
#   • .m / .mm / .cpp 自动加入 Sources build phase
#
# remove 时：
#   • basename 形如 "Foo.mm"
#   • 从所有 group / build phase 中移除

require 'xcodeproj'

SCRIPT_DIR   = File.expand_path(File.dirname(__FILE__))
PROJECT_DIR  = File.expand_path('..', SCRIPT_DIR)
PROJECT_PATH = File.join(PROJECT_DIR, 'ios-oc-mvvm.xcodeproj')
TARGET_NAME  = 'ios-oc-mvvm'

def die(msg)
  warn "[ERR] #{msg}"
  exit 1
end

def info(msg); puts "[INFO] #{msg}"; end
def ok(msg);   puts "[OK]   #{msg}"; end

def source_file?(path)
  %w[.m .mm .cpp].include?(File.extname(path))
end

def file_type_for(path)
  case File.extname(path)
  when '.mm'  then 'sourcecode.cpp.objcpp'
  when '.m'   then 'sourcecode.c.objc'
  when '.cpp' then 'sourcecode.cpp.cpp'
  when '.hpp' then 'sourcecode.cpp.h'
  when '.h'   then 'sourcecode.c.h'
  when '.plist' then 'text.plist.xml'
  else 'sourcecode.c.objc'
  end
end

# 把 "a/b/c/Foo.mm" 的目录部分转成一个 group 链（从 main_group 开始），
# 复用已有 group，找不到就新建。注意：这里 project 的 main_group 已经是 pbxproj 根，
# 顶层 "ios-oc-mvvm" group 也被当成一层出现在 parts 里。
def ensure_group_chain(project, dir_parts)
  g = project.main_group
  dir_parts.each do |part|
    child = g.groups.find { |x| x.path == part || x.name == part }
    child ||= g.new_group(part, part)
    g = child
  end
  g
end

def cmd_add(project, target, rel_paths)
  added = 0
  rel_paths.each do |rel|
    basename = File.basename(rel)
    # 幂等：同名文件已存在就跳过
    if project.files.any? { |f| f.display_name == basename }
      info "已存在，跳过: #{rel}"
      next
    end
    parts = rel.split(File::SEPARATOR)
    dir_parts = parts[0..-2]
    group = ensure_group_chain(project, dir_parts)
    file_ref = group.new_reference(basename)
    file_ref.set_last_known_file_type(file_type_for(basename))
    if source_file?(basename)
      target.add_file_references([file_ref])
    end
    ok "已加入: #{rel}"
    added += 1
  end
  added
end

def cmd_remove(project, target, basenames)
  removed = 0
  basenames.each do |bn|
    refs = project.files.select { |f| f.display_name == bn }
    if refs.empty?
      info "未找到引用，跳过: #{bn}"
      next
    end
    refs.each do |ref|
      target.source_build_phase.files_references.delete(ref)
      target.source_build_phase.files.each do |bf|
        bf.remove_from_project if bf.file_ref == ref
      end
      ref.remove_from_project
    end
    ok "已移除: #{bn}"
    removed += 1
  end
  removed
end

subcommand = ARGV.shift or die('用法: pbxproj-sync.rb add|remove <args...>')
args = ARGV

die("找不到 pbxproj: #{PROJECT_PATH}") unless File.exist?(PROJECT_PATH)
project = Xcodeproj::Project.open(PROJECT_PATH)
target  = project.targets.find { |t| t.name == TARGET_NAME } or
          die("target '#{TARGET_NAME}' not found")

case subcommand
when 'add'
  die('add 至少要一个 rel-path') if args.empty?
  n = cmd_add(project, target, args)
  project.save if n.positive?
  ok "完成：#{n} 个文件添加"
when 'remove'
  die('remove 至少要一个 basename') if args.empty?
  n = cmd_remove(project, target, args)
  project.save if n.positive?
  ok "完成：#{n} 个文件移除"
else
  die("未知子命令：#{subcommand}")
end
