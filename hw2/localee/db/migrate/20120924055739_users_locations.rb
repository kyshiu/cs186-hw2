class UsersLocations < ActiveRecord::Migration
  def change
    create_table :locations_users, :id => false do |t|
      t.integer :user_id
      t.integer :location_id
    end
  end
end
