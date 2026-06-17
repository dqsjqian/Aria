#!/usr/bin/env ruby
# add-masonry-to-pbxproj.rb — 用 CocoaPods 官方 xcodeproj gem 安全地挂 Masonry。
#
# 幂等：已挂则跳过。
# 跑一次即可；Masonry 升级后可再跑。
#
# 做的事：
#   1. 新建 Group: ThirdParty/Masonry（对应磁盘路径 ios-oc-mvvm/ThirdParty/Masonry）。
#   2. 把 Masonry/*.h、*.m 加入该 Group。
#   3. 所有 .m 加入 Sources build phase。
#   4. HEADER_SEARCH_PATHS（所有 configuration）加入 ThirdParty/Masonry 目录。

require 'xcodeproj'
require 'pathname'

SCRIPT_DIR   = File.expand_path(File.dirname(__FILE__))
PROJECT_DIR  = File.expand_path('..', SCRIPT_DIR)
PROJECT_PATH = File.join(PROJECT_DIR, 'ios-oc-mvvm.xcodeproj')
MASONRY_DIR  = File.join(PROJECT_DIR, 'ios-oc-mvvm', 'ThirdParty', 'Masonry')
# path relative to the ios-oc-mvvm group (which has path = ios-oc-mvvm)
MASONRY_REL_FROM_SRC_GROUP = 'ThirdParty/Masonry'
HEADER_SEARCH_PATH_ENTRY   = '$(SRCROOT)/ios-oc-mvvm/ThirdParty/Masonry'

project = Xcodeproj::Project.open(PROJECT_PATH)
target  = project.targets.find { |t| t.name == 'ios-oc-mvvm' } or abort('target ios-oc-mvvm not found')

# 幂等：Masonry.h 已经在工程里就跳过。
if project.files.any? { |f| f.display_name == 'Masonry.h' }
  puts '[OK] Masonry 已在 pbxproj 中，无需重复添加。'
  exit 0
end

# 找到 ios-oc-mvvm group（path = "ios-oc-mvvm"）
src_group = project.main_group.groups.find { |g| g.path == 'ios-oc-mvvm' } or
  abort('找不到 ios-oc-mvvm source group')

# 新建 ThirdParty 和 Masonry 两层 group
third_party_group = src_group.groups.find { |g| g.path == 'ThirdParty' } ||
                    src_group.new_group('ThirdParty', 'ThirdParty')
masonry_group     = third_party_group.groups.find { |g| g.path == 'Masonry' } ||
                    third_party_group.new_group('Masonry', 'Masonry')

headers = Dir.entries(MASONRY_DIR).select { |f| f.end_with?('.h') }.sort
sources = Dir.entries(MASONRY_DIR).select { |f| f.end_with?('.m') }.sort

puts "[INFO] 发现 #{headers.size} 个 .h，#{sources.size} 个 .m"

headers.each do |h|
  file_ref = masonry_group.new_reference(h)
  file_ref.set_last_known_file_type('sourcecode.c.h')
end

sources.each do |m|
  file_ref = masonry_group.new_reference(m)
  file_ref.set_last_known_file_type('sourcecode.c.objc')
  target.add_file_references([file_ref])
end

# 补 HEADER_SEARCH_PATHS（所有 configuration：Debug + Release）
target.build_configurations.each do |config|
  paths = config.build_settings['HEADER_SEARCH_PATHS'] || ['$(inherited)']
  paths = [paths] if paths.is_a?(String)
  unless paths.include?(HEADER_SEARCH_PATH_ENTRY)
    paths << HEADER_SEARCH_PATH_ENTRY
    config.build_settings['HEADER_SEARCH_PATHS'] = paths
  end
end

project.save
puts "[OK] pbxproj 已更新：+#{headers.size} header / +#{sources.size} source；" \
     "Group 'ThirdParty/Masonry'；HEADER_SEARCH_PATHS 已补。"
puts '[INFO] 下一步：Xcode 中 Cmd+B 验证。'
